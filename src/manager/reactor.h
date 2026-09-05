#ifndef WINJECT_MANAGER_REACTOR_H_
#define WINJECT_MANAGER_REACTOR_H_

#include <bfc/epoll_reactor.hpp>

#include <functional>

using reactor = bfc::epoll_reactor<std::function<void()>>;

#endif  // WINJECT_MANAGER_REACTOR_H_
