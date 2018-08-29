#pragma once

// `krbn::components_manager` can be used safely in a multi-threaded environment.

#include "application_launcher.hpp"
#include "configuration_monitor.hpp"
#include "console_user_id_monitor.hpp"
#include "constants.hpp"
#include "frontmost_application_observer.hpp"
#include "grabber_alerts_monitor.hpp"
#include "grabber_client.hpp"
#include "input_source_observer.hpp"
#include "logger.hpp"
#include "menu_process_manager.hpp"
#include "receiver.hpp"
#include "session.hpp"
#include "system_preferences_monitor.hpp"
#include "updater_process_manager.hpp"
#include "version_monitor.hpp"
#include "version_monitor_utility.hpp"
#include <thread>

namespace krbn {
class components_manager final {
public:
  components_manager(const components_manager&) = delete;

  components_manager(void) {
    queue_ = std::make_unique<thread_utility::queue>();

    version_monitor_ = version_monitor_utility::make_version_monitor_stops_main_run_loop_when_version_changed();
    start_grabber_alerts_monitor();

    console_user_id_monitor_ = std::make_unique<console_user_id_monitor>();

    console_user_id_monitor_->console_user_id_changed.connect([this](auto&& uid) {
      queue_->push_back([this, uid] {
        if (version_monitor_) {
          version_monitor_->async_manual_check();
        }

        filesystem::create_directory_with_intermediate_directories(
            constants::get_user_configuration_directory(),
            0700);

        if (uid != getuid()) {
          stop_grabber_client();
          return;
        }

        receiver_ = std::make_unique<receiver>();

        receiver_->bound.connect([this] {
          queue_->push_back([this] {
            stop_grabber_client();
            start_grabber_client();
          });
        });

        receiver_->bind_failed.connect([this](auto&& error_code) {
          queue_->push_back([this] {
            stop_grabber_client();
          });
        });

        receiver_->closed.connect([this] {
          queue_->push_back([this] {
            stop_grabber_client();
          });
        });

        receiver_->start();
      });
    });

    console_user_id_monitor_->async_start();
  }

  ~components_manager(void) {
    queue_->push_back([this] {
      stop_grabber_client();

      console_user_id_monitor_ = nullptr;
      receiver_ = nullptr;
      grabber_alerts_monitor_ = nullptr;
      version_monitor_ = nullptr;
    });

    queue_->terminate();
    queue_ = nullptr;
  }

private:
  void start_grabber_alerts_monitor(void) {
    if (grabber_alerts_monitor_) {
      return;
    }

    grabber_alerts_monitor_ = std::make_unique<grabber_alerts_monitor>(constants::get_grabber_alerts_json_file_path());

    grabber_alerts_monitor_->alerts_changed.connect([this](auto&& alerts) {
      queue_->push_back([alerts] {
        logger::get_logger().info("karabiner_grabber_alerts.json is updated.");
        if (!alerts.empty()) {
          application_launcher::launch_preferences();
        }
      });
    });

    grabber_alerts_monitor_->async_start();
  }

  void start_grabber_client(void) {
    if (grabber_client_) {
      return;
    }

    grabber_client_ = std::make_shared<grabber_client>();

    grabber_client_->connected.connect([this] {
      queue_->push_back([this] {
        if (version_monitor_) {
          version_monitor_->async_manual_check();
        }

        if (grabber_client_) {
          grabber_client_->async_connect_console_user_server();
        }

        stop_child_components();
        start_child_components();
      });
    });

    grabber_client_->connect_failed.connect([this](auto&& error_code) {
      queue_->push_back([this] {
        if (version_monitor_) {
          version_monitor_->async_manual_check();
        }

        stop_child_components();
      });
    });

    grabber_client_->closed.connect([this] {
      queue_->push_back([this] {
        if (version_monitor_) {
          version_monitor_->async_manual_check();
        }

        stop_child_components();
      });
    });

    grabber_client_->async_start();
  }

  void stop_grabber_client(void) {
    grabber_client_ = nullptr;
    stop_child_components();
  }

  void start_child_components(void) {
    configuration_monitor_ = std::make_shared<configuration_monitor>(constants::get_user_core_configuration_file_path());

    // menu_process_manager_

    menu_process_manager_ = std::make_unique<menu_process_manager>(configuration_monitor_);

    // updater_process_manager_

    updater_process_manager_ = std::make_unique<updater_process_manager>(configuration_monitor_);

    // system_preferences_monitor_

    system_preferences_monitor_ = std::make_unique<system_preferences_monitor>(configuration_monitor_);

    system_preferences_monitor_->system_preferences_changed.connect([this](auto&& system_preferences) {
      queue_->push_back([this, system_preferences] {
        if (grabber_client_) {
          grabber_client_->async_system_preferences_updated(system_preferences);
        }
      });
    });

    system_preferences_monitor_->async_start();

    // frontmost_application_observer_

    frontmost_application_observer_ = std::make_unique<frontmost_application_observer>();

    frontmost_application_observer_->frontmost_application_changed.connect([this](auto&& bundle_identifier, auto&& file_path) {
      queue_->push_back([this, bundle_identifier, file_path] {
        if (bundle_identifier == "org.pqrs.Karabiner.EventViewer" ||
            bundle_identifier == "org.pqrs.Karabiner-EventViewer") {
          return;
        }

        if (grabber_client_) {
          grabber_client_->async_frontmost_application_changed(bundle_identifier, file_path);
        }
      });
    });

    frontmost_application_observer_->async_start();

    // input_source_observer_

    input_source_observer_ = std::make_unique<input_source_observer>();

    input_source_observer_->input_source_changed.connect([this](auto&& input_source_identifiers) {
      queue_->push_back([this, input_source_identifiers] {
        if (grabber_client_) {
          grabber_client_->async_input_source_changed(input_source_identifiers);
        }
      });
    });

    input_source_observer_->async_start();

    // Start configuration_monitor_

    configuration_monitor_->async_start();
  }

  void stop_child_components(void) {
    menu_process_manager_ = nullptr;
    updater_process_manager_ = nullptr;
    system_preferences_monitor_ = nullptr;
    frontmost_application_observer_ = nullptr;
    input_source_observer_ = nullptr;

    configuration_monitor_ = nullptr;
  }

  std::unique_ptr<thread_utility::queue> queue_;

  // Core components

  std::shared_ptr<version_monitor> version_monitor_;
  std::unique_ptr<grabber_alerts_monitor> grabber_alerts_monitor_;

  std::unique_ptr<console_user_id_monitor> console_user_id_monitor_;
  std::unique_ptr<receiver> receiver_;
  std::shared_ptr<grabber_client> grabber_client_;

  // Child components

  std::shared_ptr<configuration_monitor> configuration_monitor_;
  std::unique_ptr<menu_process_manager> menu_process_manager_;
  std::unique_ptr<updater_process_manager> updater_process_manager_;
  std::unique_ptr<system_preferences_monitor> system_preferences_monitor_;
  // `frontmost_application_observer` does not work properly in karabiner_grabber after fast user switching.
  // Therefore, we have to use `frontmost_application_observer` in `console_user_server`.
  std::unique_ptr<frontmost_application_observer> frontmost_application_observer_;
  std::unique_ptr<input_source_observer> input_source_observer_;
};
} // namespace krbn