#include "application.hh"

#include <new>        // For std::nothrow
#include <stop_token> // For std::stop_source, std::stop_token, std::nostopstate
#include <thread>     // For std::jthread

#include "context.hh" // For context
#include "loader.hh"  // For loader::ui, loader::service

class impl {
public:
  impl()
      : ui_thread_(&impl::ui_thread_function, this),
        service_thread_(&impl::service_thread_function, this), 
        context_() {};
  ~impl() noexcept = default;

  void run() {
    context_.run();
  }

private:
  void ui_thread_function(std::stop_token token) {
    // Implementation of the UI thread function
    loader::ui ui_instance;
    ui_instance.set_context(&context_);
  }

  void service_thread_function(std::stop_token token) {
    // Implementation of the service thread function
    loader::service service_instance;
    service_instance.set_context(&context_);
  }

private:
  std::jthread ui_thread_;
  std::jthread service_thread_;
  context context_;
};

application::application() : pimpl_(new(std::nothrow) impl()) {}

application::~application() noexcept { delete pimpl_; }

void application::run() { pimpl_->run(); }