// Included before every C++ file of Half-Life's: <cmath>'s float functions in the global namespace,
// so that sin() of a float is sinf() and not the double one this machine has no instructions for.
#ifdef __cplusplus
#include <cmath>
using std::sin; using std::cos; using std::tan; using std::asin; using std::acos; using std::atan; using std::atan2;
using std::sqrt; using std::pow; using std::exp; using std::log; using std::fabs; using std::floor; using std::ceil; using std::fmod;
#endif
