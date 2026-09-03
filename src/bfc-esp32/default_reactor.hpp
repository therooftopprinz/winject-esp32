#ifndef BFC_DEFAULT_REACTOR_HPP_
#define BFC_DEFAULT_REACTOR_HPP_

#include "bfc-esp32/select_reactor.hpp"

namespace bfc
{

template <typename cb_t = light_function<void()>>
using default_reactor = select_reactor<cb_t>;

}  // namespace bfc

#endif  // BFC_DEFAULT_REACTOR_HPP_
