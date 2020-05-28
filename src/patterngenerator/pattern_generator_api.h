#ifndef PATTERNGENERATOR_API_H
#define PATTERNGENERATOR_API_H

#include "pattern_generator.h"
#include "apiObject.hpp"

namespace adiscope {
namespace logic {
class PatternGenerator_API : public ApiObject
{
	Q_OBJECT
public:
	explicit PatternGenerator_API(logic::PatternGenerator *pattern)
	        : ApiObject()
	        , m_pattern(pattern) {}
	~PatternGenerator_API() {}

private:
	logic::PatternGenerator *m_pattern;
};
} // namespace logic
} // namespace adiscope
#endif // PATTERNGENERATOR_API_H
