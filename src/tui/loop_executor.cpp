// SPDX-License-Identifier: Apache-2.0

#include <core/async/ExecutorContext.hpp>
#include <exception>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/tui/loop_executor.hpp>
#include <utility>

namespace morph::tui {

LoopExecutor::LoopExecutor(::core::net::EventLoop& loop)
    : _loop{&loop}, _alive{std::make_shared<LoopExecutor*>(this)} {}

LoopExecutor::~LoopExecutor() = default;

void LoopExecutor::post(std::function<void()> task) {
    _loop->post([alive = std::weak_ptr<LoopExecutor*>{_alive}, task = std::move(task)] {
        auto const self = alive.lock();
        if (!self) {
            return;
        }
        ::core::async::ExecutorScope const scope{(*self)->coreExecutor()};
        // Caught here: one exception escaping a loop turn would end the frontend's event loop.
        try {
            task();
        } catch (std::exception const& failure) {
            ::morph::log::logError("[tui] loop task threw: {}", failure.what());
        } catch (...) {
            ::morph::log::logError("[tui] loop task threw an unknown exception");
        }
    });
}

}  // namespace morph::tui
