#include "pattern_generator.h"

#include "ui_pattern_generator.h"
#include "digitalchannel_manager.hpp"
#include "dynamicWidget.hpp"
#include "basemenu.h"
#include "../logicanalyzer/logicgroupitem.h"

#include "../logicanalyzer/logicdatacurve.h"
#include "patterns/patterns.hpp"
#include "../logicanalyzer/annotationcurve.h"
#include "../logicanalyzer/annotationdecoder.h"
#include "pattern_generator_api.h"

using namespace adiscope;
using namespace adiscope::logic;

constexpr int MAX_BUFFER_SIZE = 1024 * 1024; // 1M
constexpr int DIGITAL_NR_CHANNELS = 16;
constexpr int MAX_SAMPLE_RATE = 100000000;

namespace detail {
int gcd(int a, int b)
{
	for (;;) {
		if (!a) { return b; }
		b %= a;
		if (!b) { return a; }
		a %= b;
	}
}

int lcm(int a, int b)
{
	int temp = gcd(a, b);

	return temp ? (a / temp * b) : 0;
}

} // namespace detail

PatternGenerator::PatternGenerator(M2kDigital *m2kDigital, Filter *filt,
				   ToolMenuItem *toolMenuItem, QJSEngine *engine,
				   DIOManager *diom, ToolLauncher *parent)
	: LogicTool(nullptr, toolMenuItem, new PatternGenerator_API(this), "Pattern Generator", parent)
	, m_ui(new Ui::PatternGenerator)
	, m_plot(this, 16, 10)
	, m_plotScrollBar(new QScrollBar(Qt::Vertical, this))
	, m_selectedChannel(-1)
	, m_nbChannels(DIGITAL_NR_CHANNELS)
	, m_currentGroupMenu(nullptr)
	, m_m2kDigital(m2kDigital)
	, m_bufferSize(1)
	, m_sampleRate(1)
	, m_diom(diom)
	, m_outputMode(0)
	, m_isRunning(false)
	, m_singleTimer(new QTimer(this))
{
	setupUi();

	connectSignalsAndSlots();

	m_plot.setLeftVertAxesCount(1);

	for (uint8_t i = 0; i < DIGITAL_NR_CHANNELS; ++i) {
		QCheckBox *channelBox = new QCheckBox("DIO " + QString::number(i));
		m_ui->channelEnumeratorLayout->addWidget(channelBox, i % 8, i / 8);

		channelBox->setChecked(true);

		// 1 for each channel
		// m_plot.addGenericPlotCurve()
		LogicDataCurve *curve = new LogicDataCurve(nullptr, i, this);
		curve->setTraceHeight(25);
		m_plot.addDigitalPlotCurve(curve, true);
		curve->setDisplaySampling(true);

		// use direct connection we want the processing
		// of the available data to be done in the capture thread
		connect(this, &PatternGenerator::dataAvailable, this,
			[=](uint64_t from, uint64_t to){
			curve->dataAvailable(from, to);
		}, Qt::DirectConnection);

		m_plotCurves.push_back(curve);

		connect(channelBox, &QCheckBox::toggled, [=](bool toggled){
			m_plot.enableDigitalPlotCurve(i, toggled);
			m_plot.setOffsetWidgetVisible(i, toggled);
			m_plot.positionInGroupChanged(i, 0, 0);
			m_plot.replot();

			checkEnabledChannels();
		});
		channelBox->setChecked(false);
	}

	// Add propper zoomer
	m_plot.addZoomer(0);
	m_plot.setZoomerParams(true, 20);
	m_plot.zoomBaseUpdate();

	m_plot.enableXaxisLabels();
	m_plotScrollBar->setRange(0, 100);

	setupPatterns();

	checkEnabledChannels();

	// TODO:
	// readPreferences()

	m_ui->btnGeneralSettings->setChecked(true);

	// API load
	api->setObjectName(QString::fromStdString(Filter::tool_name(
							  TOOL_PATTERN_GENERATOR)));
	api->load(*settings);
	api->js_register(engine);
}

PatternGenerator::~PatternGenerator()
{
	if (saveOnExit) {
		api->save(*settings);
	}

	delete api;

	disconnect(prefPanel, &Preferences::notify, this, &PatternGenerator::readPreferences);

	if (m_isRunning) {
		startStop(false);
	}

	for (auto &curve : m_plotCurves) {
		m_plot.removeDigitalPlotCurve(curve);
		delete curve;
	}

	if (m_buffer) {
		delete[] m_buffer;
		m_buffer = nullptr;
	}

	delete m_ui;
}

void PatternGenerator::setupUi()
{
	m_ui->setupUi(this);

	// disable cursors
	m_ui->btnCursors->setVisible(false);
	m_ui->cursorsBox->setVisible(false);

	// disable trigger menu
	m_ui->triggerLabel->setVisible(false);
	m_ui->btnTrigger->setVisible(false);

	// disable plot legend
	m_plot.disableLegend();

	// disable time trigger
	m_plot.enableTimeTrigger(false);


	int gsettings_panel = m_ui->stackedWidget->indexOf(m_ui->generalSettings);
	m_ui->btnGeneralSettings->setProperty("id", QVariant(-gsettings_panel));

//	/* Cursors Settings */
//	ui->btnCursors->setProperty("id", QVariant(-1));

//	/* Trigger Settings */
//	int triggers_panel = ui->stackedWidget->indexOf(ui->triggerSettings);
//	ui->btnTrigger->setProperty("id", QVariant(-triggers_panel));

	/* Channel Settings */
	int channelSettings_panel = m_ui->stackedWidget->indexOf(m_ui->channelSettings);
	m_ui->btnChannelSettings->setProperty("id", QVariant(-channelSettings_panel));

	// set default menu width to 0
	m_ui->rightMenu->setMaximumWidth(0);

	QSpacerItem *plotSpacer = new QSpacerItem(0, 5,
		QSizePolicy::Fixed, QSizePolicy::Fixed);

	m_ui->gridLayoutPlot->addWidget(m_plot.topArea(), 0, 0, 1, 4);
	m_ui->gridLayoutPlot->addWidget(m_plot.topHandlesArea(), 1, 0, 1, 4);

	m_ui->gridLayoutPlot->addWidget(m_plot.leftHandlesArea(), 0, 0, 4, 1);
	m_ui->gridLayoutPlot->addWidget(m_plot.rightHandlesArea(), 0, 3, 4, 1);


	m_ui->gridLayoutPlot->addWidget(&m_plot, 2, 1, 1, 1);
	m_ui->gridLayoutPlot->addWidget(m_plotScrollBar, 2, 5, 1, 1);

	m_ui->gridLayoutPlot->addWidget(m_plot.bottomHandlesArea(), 3, 0, 1, 4);
	m_ui->gridLayoutPlot->addItem(plotSpacer, 4, 0, 1, 4);

	// TODO: investigate why the plot's canvas background color is weird
	m_plot.canvas()->setStyleSheet("background-color: #272730");

	m_plot.enableAxis(QwtPlot::yLeft, false);
	m_plot.enableAxis(QwtPlot::xBottom, false);

	m_plot.setUsingLeftAxisScales(false);
	m_plot.enableLabels(false);

	m_menuOrder.push_back(m_ui->btnChannelSettings);

	m_ui->nameLineEdit->setDisabled(true);
	m_ui->nameLineEdit->setText("");
	m_ui->traceHeightLineEdit->setDisabled(true);
	m_ui->traceHeightLineEdit->setText(QString::number(1));
	m_ui->patternComboBox->setDisabled(true);
	m_ui->btnOutputMode->setDisabled(true);

	m_ui->traceHeightLineEdit->setValidator(new QIntValidator(1, 100, m_ui->traceHeightLineEdit));

	m_ui->groupWidget->setVisible(false);

	loadTriggerMenu();
}

void PatternGenerator::on_btnChannelSettings_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);

	if (checked && m_selectedChannel != -1) {
		m_ui->nameLineEdit->setText(m_plot.getChannelName(m_selectedChannel));
		m_ui->traceHeightLineEdit->setText(QString::number(
							 m_plotCurves[m_selectedChannel]->getTraceHeight()));
	}
}

void PatternGenerator::on_btnSettings_clicked(bool checked)
{
	CustomPushButton *btn = nullptr;

	if (checked && !m_menuOrder.isEmpty()) {
		btn = m_menuOrder.back();
		m_menuOrder.pop_back();
	} else {
		btn = static_cast<CustomPushButton *>(
			m_ui->settings_group->checkedButton());
	}

	btn->setChecked(checked);
}

void PatternGenerator::on_btnGeneralSettings_toggled(bool checked)
{
	triggerRightMenuToggle(
		static_cast<CustomPushButton *>(QObject::sender()), checked);
	if(checked) {
		m_ui->btnSettings->setChecked(!checked);
	}
}

void PatternGenerator::rightMenuFinished(bool opened)
{
	Q_UNUSED(opened)

	// At the end of each animation, check if there are other button check
	// actions that might have happened while animating and execute all
	// these queued actions
	while (m_menuButtonActions.size()) {
		auto pair = m_menuButtonActions.dequeue();
		toggleRightMenu(pair.first, pair.second);
	}
}

void PatternGenerator::channelSelectedChanged(int chIdx, bool selected)
{
	QSignalBlocker nameLineEditBlocker(m_ui->nameLineEdit);
	QSignalBlocker traceHeightLineEditBlocker(m_ui->traceHeightLineEdit);
	if (m_selectedChannel != chIdx && selected) {
		if (!m_ui->btnChannelSettings->isChecked()) {
			m_ui->btnChannelSettings->setChecked(true);
		}

		m_selectedChannel = chIdx;
		m_ui->nameLineEdit->setEnabled(true);
		m_ui->nameLineEdit->setText(m_plotCurves[m_selectedChannel]->getName());
		m_ui->traceHeightLineEdit->setEnabled(true);
		m_ui->traceHeightLineEdit->setText(
					QString::number(m_plotCurves[m_selectedChannel]->getTraceHeight()));
		m_ui->patternComboBox->setEnabled(true);
		m_ui->btnOutputMode->setEnabled(true);

		updateChannelGroupWidget(true);
		updateChannelGroupPattern(true);


	} else if (m_selectedChannel == chIdx && !selected) {
		m_selectedChannel = -1;
		m_ui->nameLineEdit->setDisabled(true);
		m_ui->nameLineEdit->setText("");
		m_ui->traceHeightLineEdit->setDisabled(true);
		m_ui->traceHeightLineEdit->setText("");
		m_ui->patternComboBox->setDisabled(true);
		m_ui->btnOutputMode->setDisabled(true);

		updateChannelGroupWidget(false);
		updateChannelGroupPattern(false);

	}
}

void PatternGenerator::on_btnGroupChannels_toggled(bool checked)
{
	m_ui->btnGroupChannels->setText(checked ? "Done" : "Group");

	if (checked) {
		m_plot.beginGroupSelection();
	} else {
		if (m_plot.endGroupSelection(true)) {
			channelSelectedChanged(m_selectedChannel, false);
			updateGroupsAndPatterns();
		}
	}
}

void PatternGenerator::removeAnnotationCurveOfPattern(PatternUI *pattern)
{
	if (m_annotationCurvePatternUiMap.contains(pattern)) {
		disconnect(m_annotationCurvePatternUiMap[pattern].second);

		GenericLogicPlotCurve *curve = m_annotationCurvePatternUiMap[pattern].first;

		bool dummy = false;
		int removeIndx = m_plotCurves.indexOf(curve);
		m_plot.removeFromGroup(m_selectedChannel,
				       m_plot.getGroupOfChannel(m_selectedChannel).indexOf(removeIndx),
				       dummy);

		m_plot.removeDigitalPlotCurve(curve);

		m_plotCurves.removeOne(curve);

		delete curve;

		m_annotationCurvePatternUiMap.remove(pattern);
	}
}

void PatternGenerator::updateAnnotationCurveChannelsForPattern(const QPair<QVector<int>, PatternUI *> &pattern)
{
	if (pattern.second->getAnnotationCurve()) {
		AnnotationCurve *curve = dynamic_cast<AnnotationCurve *>(
					pattern.second->getAnnotationCurve());
		QVector<int> chToAssign = pattern.second->getChannelsToAssign();
		int skipped = 0;
		for (int i = 0; i < curve->getAnnotationDecoder()->getNrOfChannels(); ++i) {
			if (!chToAssign.empty() && !chToAssign.contains(i)) {
				skipped++;
				continue;
			}
			if (i - skipped >= pattern.first.size()) { break; }
			if (pattern.first[i - skipped] >= DIGITAL_NR_CHANNELS) { continue; }
			curve->getAnnotationDecoder()->assignChannel(i, pattern.first[i - skipped]);
		}
	}
}

void PatternGenerator::patternSelected(const QString &pattern, int ch, const QString &json)
{
	int selected = ch != -1 ? ch : m_selectedChannel;
	if (pattern != "-") {
		qDebug() << "Selected: " << pattern;
		Pattern *patternObj = nullptr;
		if (pattern == "") {
			patternObj = Pattern_API::fromString(json);
		} else {
			patternObj = PatternFactory::create(pattern);
		}

		PatternUI *patternUi = PatternFactory::create_ui(patternObj, m_ui->patternWidget);
		m_ui->patternLayout->addWidget(patternUi);
		patternUi->setVisible(true);

		connect(patternUi, &PatternUI::patternParamsChanged,
			this, &PatternGenerator::regenerate);
		connect(patternUi, &PatternUI::patternParamsChanged,
			this, &PatternGenerator::generateBuffer);

		bool didSet = false;
		for (auto &ep : m_enabledPatterns) {
			if (ep.first.contains(selected)) {
				m_ui->patternLayout->removeWidget(ep.second);

				removeAnnotationCurveOfPattern(ep.second);

				if (patternUi->getDecoder()) {
					qDebug() << "This pattern has a decoder!";

					AnnotationCurve *curve = new AnnotationCurve(this, patternUi->getDecoder());
					curve->setTraceHeight(25);
					m_plot.addDigitalPlotCurve(curve, true);

					patternUi->setAnnotationCurve(curve);

					// use direct connection we want the processing
					// of the available data to be done in the capture thread
					QMetaObject::Connection connectionHandle = connect(this, &PatternGenerator::dataAvailable,
						this, [=](uint64_t from, uint64_t to){
						curve->dataAvailable(from, to);
					}, Qt::DirectConnection);

					m_plotCurves.push_back(curve);

					m_plot.addToGroup(selected, m_plotCurves.size() - 1);
		//			m_plot.setOffsetHandleVisible(m_plotCurves.size() - 1, false);
					m_annotationCurvePatternUiMap[patternUi] = { curve, connectionHandle };

					m_plot.setChannelSelectable(m_plotCurves.size() - 1, false);
				}

				ep.second->deleteLater();
				ep.second = patternUi;
				didSet = true;

				// TODO: remove annotation curve if it has one!!!

				patternUi->build_ui(m_ui->patternWidget, 0);
				patternUi->get_pattern()->init();
				patternUi->post_load_ui();

				break;
			}
		}

		if (!didSet) {
			// we have a new <group, pattern> configuration
			QVector<int> group = m_plot.getGroupOfChannel(selected);
			if (group.empty()) {
				m_enabledPatterns.push_back({{selected}, patternUi});

			} else {
				m_enabledPatterns.push_back({group, patternUi});
			}

			if (patternUi->getDecoder()) {
				qDebug() << "This pattern has a decoder!";

				AnnotationCurve *curve = new AnnotationCurve(this, patternUi->getDecoder());
				curve->setTraceHeight(25);
				m_plot.addDigitalPlotCurve(curve, true);

				patternUi->setAnnotationCurve(curve);

				// use direct connection we want the processing
				// of the available data to be done in the capture thread
				QMetaObject::Connection connectionHandle = connect(this, &PatternGenerator::dataAvailable,
					this, [=](uint64_t from, uint64_t to){
					curve->dataAvailable(from, to);
				}, Qt::DirectConnection);

				m_plotCurves.push_back(curve);

				m_plot.addToGroup(selected, m_plotCurves.size() - 1);
	//			m_plot.setOffsetHandleVisible(m_plotCurves.size() - 1, false);
				m_annotationCurvePatternUiMap[patternUi] = { curve, connectionHandle };

				m_plot.setChannelSelectable(m_plotCurves.size() - 1, false);
			}

			patternUi->build_ui(m_ui->patternWidget, 0);
			patternUi->get_pattern()->init();
			patternUi->post_load_ui();
		}
	} else {
		int remove = -1;
		for (int i = 0; i < m_enabledPatterns.size(); ++i) {
			if (m_enabledPatterns[i].first.contains(selected)) {
				remove = i;
				break;
			}
		}
		if (remove != -1) {

			// TODO: remove annotation curve if it has one!!!

			m_ui->patternLayout->removeWidget(m_enabledPatterns[remove].second);

			removeAnnotationCurveOfPattern(m_enabledPatterns[remove].second);

			m_enabledPatterns[remove].second->deleteLater();
			m_enabledPatterns.removeAt(remove);
		}
	}

	generateBuffer();
	regenerate(); // will start/stop if running!
}

void PatternGenerator::on_btnOutputMode_toggled(bool checked)
{
	if (m_selectedChannel >= DIGITAL_NR_CHANNELS) {
		return;
	}

	m_outputMode ^= (-checked ^ m_outputMode) & (1 << m_selectedChannel);
	m_diom->setOutputMode(m_selectedChannel, checked);
}

uint64_t PatternGenerator::computeSampleRate() const
{
	uint64_t sr = 1;
	for (const QPair<QVector<int>, PatternUI *> &pattern : m_enabledPatterns) {
		const uint64_t patternSamplingFrequency = pattern.second->get_pattern()->get_min_sampling_freq();

		if (!patternSamplingFrequency) {
			continue;
		}

		sr = detail::lcm(patternSamplingFrequency, sr);
	}

	if (!sr || sr > MAX_SAMPLE_RATE) {
		sr = MAX_SAMPLE_RATE;
	} else {
		sr = MAX_SAMPLE_RATE / (MAX_SAMPLE_RATE / sr);
	}

	return sr;
}

uint64_t PatternGenerator::computeBufferSize(uint64_t sampleRate) const
{
	const uint64_t divconst = 50000000 / 256;
	uint64_t size = sampleRate / divconst;
	uint64_t minSize = 4;
	if (size < minSize) {
		size = minSize;
	}

	uint64_t bufferSize = detail::lcm(size, 4);
	uint64_t maxNonPeriodic = size;

	for (const QPair<QVector<int>, PatternUI *> &pattern : m_enabledPatterns) {
		const uint64_t patternBufferSize = pattern.second->get_pattern()
				->get_required_nr_of_samples(sampleRate, pattern.first.size());

		qDebug() << patternBufferSize;

		if (!patternBufferSize) {
			continue;
		}

		if (pattern.second->get_pattern()->is_periodic()) {
			bufferSize = detail::lcm(patternBufferSize, bufferSize);
		} else {
			if (maxNonPeriodic < patternBufferSize) {
				maxNonPeriodic = patternBufferSize;
			}
		}
	}

	if (maxNonPeriodic > bufferSize) {
		uint64_t result = 1;

		for (int i = 1; result < MAX_BUFFER_SIZE && maxNonPeriodic > result; ++i) {
			result = bufferSize * i;
		}

		bufferSize = result;
	}

	return bufferSize;
}

uint16_t PatternGenerator::remapBuffer(uint8_t *mapping, uint32_t val)
{
	short ret=0;
	int i=0;

	while (val) {
		if (val&0x01) {
			ret = ret | (1<<mapping[i]);
		}

		i++;
		val>>=1;
	}

	return ret;
}

void PatternGenerator::commitBuffer(const QPair<QVector<int>, PatternUI *> &pattern,
				    uint16_t *buffer,
				    uint32_t bufferSize)
{
	uint8_t channelMapping[16];
	memset(channelMapping, 0x00, 16 * sizeof(uint8_t));
	short *bufferPtr = pattern.second->get_pattern()->get_buffer();

	uint16_t chgMask = 0;

	auto bufferChannelMask = (1 << pattern.first.size()) - 1;
	for (int i = 0; i < pattern.first.size(); ++i) {
		channelMapping[i] = pattern.first[i];
		chgMask = chgMask | (1 << pattern.first[i]);
	}

	for (int i = 0; i < bufferSize; ++i) {
		auto val = (bufferPtr[i] & bufferChannelMask);
		buffer[i] = (buffer[i] & ~(chgMask)) | remapBuffer(channelMapping, val);
	}
}

void PatternGenerator::checkEnabledChannels()
{
	bool foundOneEnabled = false;
	for (int i = 0; i < m_plotCurves.size(); ++i) {
		const bool enabled = !!m_plotCurves[i]->plot();
		if (enabled) {
			foundOneEnabled = true;
			break;
		}
	}

	m_ui->runSingleWidget->setEnabled(foundOneEnabled);
	runButton()->setEnabled(foundOneEnabled);
	setDynamicProperty(runButton(), "disabled", !foundOneEnabled);
}

void PatternGenerator::regenerate()
{
	if (m_isRunning) {
		startStop(false);
		startStop(true);
	}
}

void PatternGenerator::startStop(bool start)
{
	qDebug() << "Started status: " << start;
	if (start) {

		if (m_singleTimer->isActive()) {
			m_singleTimer->stop();
			startStop(false);
		}

		const bool isSingle = m_ui->runSingleWidget->singleButtonChecked();

		generateBuffer();

		m_plot.replot();

		uint16_t lockMask = 0;
		for (int i = 0; i < DIGITAL_NR_CHANNELS; ++i) {
			bool enabled = !!m_plotCurves[i]->plot();
			lockMask = lockMask | enabled << i;
			if (enabled) {
				m_m2kDigital->setDirection(i, DIO_OUTPUT);
				m_m2kDigital->enableChannel(i, true);
			}
		}

		m_diom->setMode(m_outputMode);

		m_diom->lock(lockMask);
		try {

			m_m2kDigital->setSampleRateOut(m_sampleRate);
			m_m2kDigital->setCyclic(!isSingle);
			m_m2kDigital->push(m_buffer, m_bufferSize);

			if (isSingle) {
				QSignalBlocker blocker(m_ui->runSingleWidget);
				QSignalBlocker runBtnBlocker(runButton());
				runButton()->setChecked(false);
				m_ui->runSingleWidget->toggle(false);
				start = false;
				const double timeout = 500.0 + static_cast<double>(m_bufferSize) /
						static_cast<double>(m_sampleRate) * 500.0;
				m_singleTimer->singleShot(timeout, [=]() {
					try {
						m_diom->unlock();
						m_m2kDigital->stopBufferOut();

						for (int i = 0; i < DIGITAL_NR_CHANNELS; ++i) {
							bool enabled = !!m_plotCurves[i]->plot();
							if (enabled) {
								m_m2kDigital->enableChannel(i, false);
							}
						}
					} catch (std::exception &e) {
						qDebug() << e.what();
					}
				});
			}

		} catch (std::exception &e) {
			qDebug() << e.what();
		}

	} else {
		try {
			m_diom->unlock();
			m_m2kDigital->cancelBufferOut();
			m_m2kDigital->stopBufferOut();

			for (int i = 0; i < DIGITAL_NR_CHANNELS; ++i) {
				bool enabled = !!m_plotCurves[i]->plot();
				if (enabled) {
					m_m2kDigital->enableChannel(i, false);
				}
			}
		} catch (std::exception &e) {
			qDebug() << e.what();
		}
	}

	m_isRunning = start;
}

void PatternGenerator::generateBuffer()
{
	// Compute samplerate
	const uint64_t sr = computeSampleRate();

	qDebug() << "Sample rate is: " << sr;
	m_sampleRate = sr;

	const uint64_t bufferSize = computeBufferSize(sr);

	qDebug() << "Buffer size is: " << bufferSize;
	m_bufferSize = bufferSize;

	if (m_buffer) {
		delete[] m_buffer;
	}

	m_buffer = new uint16_t[bufferSize];
	memset(m_buffer, 0x0000, bufferSize * sizeof(uint16_t));

	for (int i = 0; i < m_plotCurves.size(); ++i) {
		QwtPlotCurve *curve = m_plot.getDigitalPlotCurve(i);
		GenericLogicPlotCurve *logic_curve = dynamic_cast<GenericLogicPlotCurve *>(curve);
		logic_curve->reset();

		logic_curve->setSampleRate(sr);
		logic_curve->setBufferSize(bufferSize);
		logic_curve->setTimeTriggerOffset(0);
	}

	m_plot.setHorizUnitsPerDiv(1.0 / m_sampleRate * m_bufferSize / 16.0);
	m_plot.setHorizOffset(1.0 / m_sampleRate * m_bufferSize / 2.0);
	m_plot.cancelZoom();
	m_plot.zoomBaseUpdate();
	m_plot.replot();

	for (QPair<QVector<int>, PatternUI *> &pattern : m_enabledPatterns) {
		pattern.second->get_pattern()->generate_pattern(sr, bufferSize, pattern.first.size());
		commitBuffer(pattern, m_buffer, bufferSize);
		pattern.second->get_pattern()->delete_buffer();
		updateAnnotationCurveChannelsForPattern(pattern);
	}

	Q_EMIT dataAvailable(0, bufferSize);
}

void PatternGenerator::triggerRightMenuToggle(CustomPushButton *btn, bool checked)
{
	// Queue the action, if right menu animation is in progress. This way
	// the action will be remembered and performed right after the animation
	// finishes
	if (m_ui->rightMenu->animInProgress()) {
		m_menuButtonActions.enqueue(
			QPair<CustomPushButton *, bool>(btn, checked));
	} else {
		toggleRightMenu(btn, checked);
	}
}

void PatternGenerator::toggleRightMenu(CustomPushButton *btn, bool checked)
{
	int id = btn->property("id").toInt();

	if (id != -m_ui->stackedWidget->indexOf(m_ui->generalSettings)){
		if (!m_menuOrder.contains(btn)){
			m_menuOrder.push_back(btn);
		} else {
			m_menuOrder.removeOne(btn);
			m_menuOrder.push_back(btn);
		}
	}

	if (checked) {
		settingsPanelUpdate(id);
	}

	m_ui->rightMenu->toggleMenu(checked);
}

void PatternGenerator::settingsPanelUpdate(int id)
{
	if (id >= 0) {
		m_ui->stackedWidget->setCurrentIndex(0);
	} else {
		m_ui->stackedWidget->setCurrentIndex(-id);
	}

	for (int i = 0; i < m_ui->stackedWidget->count(); i++) {
		QSizePolicy::Policy policy = QSizePolicy::Ignored;

		if (i == m_ui->stackedWidget->currentIndex()) {
			policy = QSizePolicy::Expanding;
		}
		QWidget *widget = m_ui->stackedWidget->widget(i);
		widget->setSizePolicy(policy, policy);
	}
	m_ui->stackedWidget->adjustSize();
}

void PatternGenerator::connectSignalsAndSlots()
{

	connect(m_ui->runSingleWidget, &RunSingleWidget::toggled,
		[=](bool checked){
		disconnect(run_button, &QPushButton::toggled,
			m_ui->runSingleWidget, &RunSingleWidget::toggle);
		auto btn = dynamic_cast<CustomPushButton *>(run_button);
		btn->setChecked(checked);
		connect(run_button, &QPushButton::toggled,
			m_ui->runSingleWidget, &RunSingleWidget::toggle);
//		if (!checked) {
//			m_plot.setTriggerState(CapturePlot::Stop);
//		}
	});
	connect(run_button, &QPushButton::toggled,
		m_ui->runSingleWidget, &RunSingleWidget::toggle);
	connect(m_ui->runSingleWidget, &RunSingleWidget::toggled,
		this, &PatternGenerator::startStop);


	connect(m_ui->rightMenu, &MenuAnim::finished,
		this, &PatternGenerator::rightMenuFinished);

	connect(&m_plot, &CapturePlot::channelSelected,
		this, &PatternGenerator::channelSelectedChanged);


	connect(m_plotScrollBar, &QScrollBar::valueChanged, [=](double value) {
		m_plot.setYaxis(-5 - (value * 0.05), 5 - (value * 0.05));
		m_plot.replot();
	});

	connect(m_ui->traceHeightLineEdit, &QLineEdit::textChanged, [=](const QString &text){
		auto validator = m_ui->traceHeightLineEdit->validator();
		QString toCheck = text;
		int pos;

		setDynamicProperty(m_ui->traceHeightLineEdit,
				   "invalid",
				   validator->validate(toCheck, pos) == QIntValidator::Intermediate);
	});

	connect(m_ui->traceHeightLineEdit, &QLineEdit::editingFinished, [=](){
		int value = m_ui->traceHeightLineEdit->text().toInt();
		m_plotCurves[m_selectedChannel]->setTraceHeight(value);
		m_plot.replot();
		m_plot.positionInGroupChanged(m_selectedChannel, 0, 0);
	});

	connect(m_ui->nameLineEdit, &QLineEdit::textChanged, [=](const QString &text){
		m_plot.setChannelName(text, m_selectedChannel);
		m_plotCurves[m_selectedChannel]->setName(text);
		if (m_selectedChannel < m_nbChannels) {
			QWidget *widgetInLayout = m_ui->channelEnumeratorLayout->itemAtPosition(m_selectedChannel % 8,
								    m_selectedChannel / 8)->widget();
			auto channelBox = dynamic_cast<QCheckBox *>(widgetInLayout);
			channelBox->setText(text);
		}
	});

	connect(m_ui->printBtn, &QPushButton::clicked, [=](){
		m_plot.printWithNoBackground("Pattern Generator");
	});
}

void PatternGenerator::updateChannelGroupWidget(bool visible)
{
	QVector<int> channelsInGroup = m_plot.getGroupOfChannel(m_selectedChannel);

	int channelsInGroupSize = channelsInGroup.size();
	for (int i = 0; i < channelsInGroup.size(); ++i) {
		if (channelsInGroup[i] >= DIGITAL_NR_CHANNELS) {
			channelsInGroupSize--;
		}
	}

	const bool shouldBeVisible = visible & (channelsInGroupSize > 1);
	m_ui->groupWidget->setVisible(shouldBeVisible);

	if (!shouldBeVisible) {
		return;
	}

	if (channelsInGroup == m_currentGroup) {
		return;
	}

	m_currentGroup = channelsInGroup;

	if (m_currentGroupMenu) {
		m_ui->groupWidgetLayout->removeWidget(m_currentGroupMenu);
		m_currentGroupMenu->deleteLater();
		m_currentGroupMenu = nullptr;
	}

	m_currentGroupMenu = new BaseMenu(m_ui->groupWidget);
	m_ui->groupWidgetLayout->addWidget(m_currentGroupMenu);

	connect(m_currentGroupMenu, &BaseMenu::itemMovedFromTo, [=](short from, short to){
		m_plot.positionInGroupChanged(m_selectedChannel, from, to);
		channelInGroupChangedPosition(from, to);
	});

	for (int i = 0; i < channelsInGroup.size(); ++i) {
		if (channelsInGroup[i] >= DIGITAL_NR_CHANNELS) {
			continue;
		}

		QString name = m_plotCurves[channelsInGroup[i]]->getName();
		LogicGroupItem *item = new LogicGroupItem(name, m_currentGroupMenu);
		connect(m_plotCurves[channelsInGroup[i]], &GenericLogicPlotCurve::nameChanged,
				item, &LogicGroupItem::setName);
		connect(item, &LogicGroupItem::deleteBtnClicked, [=](){
			bool groupDeleted = false;
			m_plot.removeFromGroup(m_selectedChannel, item->position(), groupDeleted);

			qDebug() << "m_selectedChannel: " << m_selectedChannel << " deleted: " << m_currentGroup[item->position()];
			if (m_selectedChannel == m_currentGroup[item->position()] && !groupDeleted) {
				m_ui->groupWidget->setVisible(false);
			}

			m_currentGroup.removeAt(item->position());
			if (groupDeleted) {
				m_ui->groupWidget->setVisible(false);
				m_currentGroup.clear();
				m_ui->groupWidgetLayout->removeWidget(m_currentGroupMenu);
				m_currentGroupMenu->deleteLater();
				m_currentGroupMenu = nullptr;
			}
			channelInGroupRemoved(item->position());
		});
		m_currentGroupMenu->insertMenuItem(item);
	}

	m_currentGroupMenu->setMaximumHeight(channelsInGroup.size() * 27);
}

void PatternGenerator::setupPatterns()
{
	PatternFactory::init();
	m_ui->patternComboBox->addItem("-"); // No pattern selected yet
	m_ui->patternComboBox->addItems(PatternFactory::get_ui_list());

	connect(m_ui->patternComboBox, &QComboBox::currentTextChanged,
		[=](const QString &pattern){
		patternSelected(pattern);
	});
}

void PatternGenerator::updateChannelGroupPattern(bool visible)
{
	QString pattern = "-";

	qDebug() << "################################################################";
	qDebug() << "Update ch group pattern for: " << m_selectedChannel;
	qDebug() << "Should be visible: " << visible;
	qDebug() << "Enabled patterns: " << m_enabledPatterns;
	qDebug() << "################################################################";

	for (const auto &ep : m_enabledPatterns) {
		if (ep.first.contains(m_selectedChannel)) {
			pattern = QString::fromStdString(ep.second->get_pattern()->get_name());
			ep.second->setVisible(visible);
		} else {
			ep.second->setVisible(false);
		}
	}

	QSignalBlocker patternComboBoxBlocker(m_ui->patternComboBox);
	m_ui->patternComboBox->setCurrentText(pattern);

	const bool shouldBeVisible = visible & (pattern != "-");

	// do something with visibility
	m_ui->patternWidget->setVisible(true);
}

void PatternGenerator::updateGroupsAndPatterns()
{
	QVector<QVector<int>> allGroups = m_plot.getAllGroups();
	for (int i = 0; i < allGroups.size(); ++i) {
		PatternUI *firstFound = nullptr;
		for (int c = 0; c < allGroups[i].size(); ++c) {
			int currentIndex = 0;
			while (currentIndex < m_enabledPatterns.size()) {
				if (m_enabledPatterns[currentIndex].first.contains(allGroups[i][c])) {
					if (!firstFound) {
						firstFound = m_enabledPatterns[currentIndex].second;
					}
					m_enabledPatterns.removeAt(currentIndex);
				} else {
					currentIndex++;
				}
			}
		}
		if (firstFound) {
			m_enabledPatterns.append({allGroups[i], firstFound});
			firstFound = nullptr;
		}
	}

	generateBuffer();
	regenerate();

//	qDebug() << "#### Update groups and patterns ####";
//	qDebug() << m_enabledPatterns;
}

void PatternGenerator::channelInGroupChangedPosition(int from, int to)
{
	for (int i = 0; i < m_enabledPatterns.size(); ++i) {
		if (m_enabledPatterns[i].first.contains(m_selectedChannel)) {
			auto item = m_enabledPatterns[i].first.takeAt(from);
			m_enabledPatterns[i].first.insert(to, item);
			break;
		}
	}

	generateBuffer();
	regenerate();

//	qDebug() << "#### channel in group changed position ####";
//	qDebug() << m_enabledPatterns;
}

void PatternGenerator::channelInGroupRemoved(int position)
{
	for (int i = 0; i < m_enabledPatterns.size(); ++i) {
		if (m_enabledPatterns[i].first.contains(m_selectedChannel)) {
			m_enabledPatterns[i].first.removeAt(position);
			break;
		}
	}

	generateBuffer();
	regenerate();

//	qDebug() << "#### channel in group removed ####";
	//	qDebug() << m_enabledPatterns;
}

void PatternGenerator::loadTriggerMenu()
{
//	auto trigger = m_m2kDigital->getOutTrigger();
	// TODO: currently not available on libm2k master branch
}
