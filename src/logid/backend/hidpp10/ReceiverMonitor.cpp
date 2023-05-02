/*
 * Copyright 2019-2023 PixlOne
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <backend/hidpp10/ReceiverMonitor.h>
#include <util/task.h>
#include <util/log.h>

using namespace logid::backend::hidpp10;
using namespace logid::backend::hidpp;

ReceiverMonitor::ReceiverMonitor(const std::string& path,
                                 const std::shared_ptr<raw::DeviceMonitor>& monitor, double timeout)
        : _receiver(Receiver::make(path, monitor, timeout)) {

    Receiver::NotificationFlags notification_flags{true, true, true};
    _receiver->setNotifications(notification_flags);
}

void ReceiverMonitor::ready() {
    if (_connect_ev_handler.empty()) {
        _connect_ev_handler = _receiver->rawDevice()->addEventHandler(
                {[](const std::vector<uint8_t>& report) -> bool {
                    if (report[Offset::Type] == Report::Type::Short ||
                        report[Offset::Type] == Report::Type::Long) {
                        uint8_t sub_id = report[Offset::SubID];
                        return (sub_id == Receiver::DeviceConnection ||
                                sub_id == Receiver::DeviceDisconnection);
                    }
                    return false;
                }, [this](const std::vector<uint8_t>& raw) -> void {
                    /* Running in a new thread prevents deadlocks since the
                     * receiver may be enumerating.
                     */
                    hidpp::Report report(raw);

                    run_task([this, report, path = this->_receiver->rawDevice()->rawPath()]() {
                        if (report.subId() == Receiver::DeviceConnection) {
                            try {
                                this->addDevice(this->_receiver->deviceConnectionEvent(report));
                            } catch (std::exception& e) {
                                logPrintf(ERROR, "Failed to add device %d to receiver on %s: %s",
                                          report.deviceIndex(), path.c_str(), e.what());
                            }
                        } else if (report.subId() == Receiver::DeviceDisconnection) {
                            try {
                                this->removeDevice(
                                        this->_receiver->deviceDisconnectionEvent(report));
                            } catch (std::exception& e) {
                                logPrintf(ERROR, "Failed to remove device %d from "
                                                 "receiver on %s: %s", report.deviceIndex(),
                                          path.c_str(), e.what());
                            }
                        }
                    });
                }
                });
    }

    if (_discover_ev_handler.empty()) {
        _discover_ev_handler = _receiver->addEventHandler(
                {[](const hidpp::Report& report) -> bool {
                    return (report.subId() == Receiver::DeviceDiscovered) &&
                           (report.type() == Report::Type::Long);
                },
                 [this](const hidpp::Report& report) {
                     std::lock_guard lock(_pair_mutex);
                     if (_pair_state == Discovering) {
                         bool filled = Receiver::fillDeviceDiscoveryEvent(_discovery_event, report);

                         if (filled) {
                             _pair_state = FindingPasskey;
                             run_task([this, event = _discovery_event]() {
                                 receiver()->startBoltPairing(event);
                             });
                         }
                     }
                 }
                });
    }

    if (_passkey_ev_handler.empty()) {
        _passkey_ev_handler = _receiver->addEventHandler(
                {[](const hidpp::Report& report) -> bool {
                    return report.subId() == Receiver::PasskeyRequest &&
                        report.type() == hidpp::Report::Type::Long;
                },
                 [this](const hidpp::Report& report) {
                     std::lock_guard lock(_pair_mutex);
                     if (_pair_state == FindingPasskey) {
                         auto passkey = Receiver::passkeyEvent(report);

                         _pair_state = Pairing;
                         pairReady(_discovery_event, passkey);
                     }
                 }
                });
    }

    if (_pair_status_handler.empty()) {
        _pair_status_handler = _receiver->addEventHandler(
                {[](const hidpp::Report& report) -> bool {
                    return report.subId() == Receiver::DiscoveryStatus ||
                           report.subId() == Receiver::PairStatus ||
                           report.subId() == Receiver::BoltPairStatus;
                },
                 [this](const hidpp::Report& report) {
                     std::lock_guard lock(_pair_mutex);
                     // TODO: forward status to user
                     if (report.subId() == Receiver::DiscoveryStatus) {
                         auto event = Receiver::discoveryStatusEvent(report);

                         if (_pair_state == Discovering && !event.discovering)
                             _pair_state = NotPairing;
                     } else if (report.subId() == Receiver::PairStatus) {
                         auto event = Receiver::pairStatusEvent(report);

                         if ((_pair_state == FindingPasskey || _pair_state == Pairing) &&
                             !event.pairing)
                             _pair_state = NotPairing;
                     } else if (report.subId() == Receiver::BoltPairStatus) {
                         auto event = Receiver::boltPairStatusEvent(report);

                         if ((_pair_state == FindingPasskey || _pair_state == Pairing) &&
                             !event.pairing)
                             _pair_state = NotPairing;
                     }
                 }
                });
    }

    enumerate();
}

void ReceiverMonitor::enumerate() {
    _receiver->enumerate();
}

void ReceiverMonitor::waitForDevice(hidpp::DeviceIndex index) {
    auto handler_id = std::make_shared<EventHandlerLock<raw::RawDevice>>();

    *handler_id = _receiver->rawDevice()->addEventHandler(
            {[index](const std::vector<uint8_t>& report) -> bool {
                return report[Offset::DeviceIndex] == index;
            },
             [this, index, handler_id]([[maybe_unused]] const std::vector<uint8_t>& report) {
                 hidpp::DeviceConnectionEvent event{};
                 event.withPayload = false;
                 event.linkEstablished = true;
                 event.index = index;
                 event.fromTimeoutCheck = true;

                 run_task([this, event, handler_id]() {
                     *handler_id = {};
                     try {
                         addDevice(event);
                     } catch (std::exception& e) {
                         logPrintf(ERROR, "Failed to add device %d to receiver on %s: %s",
                                   event.index, _receiver->rawDevice()->rawPath().c_str(),
                                   e.what());
                     }
                 });
             }
            });
}

std::shared_ptr<Receiver> ReceiverMonitor::receiver() const {
    return _receiver;
}

void ReceiverMonitor::_startPair(uint8_t timeout) {
    {
        std::lock_guard lock(_pair_mutex);
        _pair_state = _receiver->bolt() ? Discovering : Pairing;
        _discovery_event = {};
    }

    if (_receiver->bolt())
        receiver()->startDiscover(timeout);
    else
        receiver()->startPairing(timeout);
}

void ReceiverMonitor::_stopPair() {
    PairState last_state;
    {
        std::lock_guard lock(_pair_mutex);
        last_state = _pair_state;
        _pair_state = NotPairing;
    }

    if (last_state == Discovering)
        receiver()->stopDiscover();
    else if (last_state == Pairing || last_state == FindingPasskey)
        receiver()->stopPairing();
}
