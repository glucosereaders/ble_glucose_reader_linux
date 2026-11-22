#include <iostream>
#include <chrono>
#include <cmath>
#include <dbus-cxx.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sys/poll.h>

#include "dbus-cxx/standard-interfaces/objectmanagerproxy.h"

#include <argh.h>

using namespace std::chrono_literals;


namespace {
    constexpr auto GLUCOSE_SERVICE_UUID = "00001808-0000-1000-8000-00805f9b34fb";
    constexpr auto RECORD_ACCESS_CONTROL_POINT_CHARACTERISTIC_UUID = "00002a52-0000-1000-8000-00805f9b34fb";
    constexpr auto GLUCOSE_MEASUREMENT_CHARACTERISTIC_UUID = "00002a18-0000-1000-8000-00805f9b34fb";
    constexpr auto GLUCOSE_MEASUREMENT_CONTEXT_CHARACTERISTIC_UUID = "00002a34-0000-1000-8000-00805f9b34fb";

    using PropertyMap = std::map<std::string, DBus::Variant>;
    using InterfaceMap = std::map<std::string, PropertyMap>;
    using ManagedObjectsMap = std::map<DBus::Path, InterfaceMap>;
    using PropertiesChangedSignature = void(
        std::string,
        PropertyMap,
        std::vector<std::string>);

    struct GlucoseDevice {
        std::string address;
        std::string name;
        std::uint32_t class_of_device;
        std::uint16_t appearance;
        DBus::Path path;
        bool paired;
        bool connected;
        bool bonded;
        bool trusted;
        bool services_resolved;
        std::string address_type;
        bool discoverable;

        std::shared_ptr<DBus::PropertyProxy<std::vector<uint8_t> > > pp;
        std::shared_ptr<DBus::ObjectProxy> op;
        std::shared_ptr<DBus::PropertyProxy<bool> > connected_pp;
        std::shared_ptr<DBus::PropertyProxy<bool> > services_resolved_pp;
        sigc::connection pp_conn;

        [[maybe_unused]] void print() const {
            std::cout << "Glucose Device:" << '\n';
            std::cout << "Address: " << address << "\n";
            std::cout << "Address Type: " << address_type << "\n";
            std::cout << "Name: " << name << "\n";
            std::cout << "Class of Device: " << class_of_device << "\n";
            std::cout << "Appearance: " << appearance << "\n";
            std::cout << "Path: " << path << "\n";
            std::cout << "Paired: " << paired << "\n";
            std::cout << "Connected: " << connected << "\n";
            std::cout << "Bonded: " << bonded << "\n";
            std::cout << "Trusted: " << trusted << "\n";
            std::cout << "Services Resolved: " << services_resolved << "\n";
            std::cout << "Discoverable: " << discoverable << "\n";
        }
    };

    struct Characteristic {
        std::string uuid{};
        std::string path{};
        bool notify_acquired{};
        bool write_acquired{};
        bool notifying{};
        bool flag_notify{};
        bool flag_read{};
        bool flag_write{};
        bool flag_indicate{};

        [[maybe_unused]] void print() const {
            std::cout << "Characteristic:" << '\n';
            std::cout << "UUID: " << uuid << "\n";
            std::cout << "Path: " << path << "\n";
            std::cout << "Notify Acquired: " << notify_acquired << "\n";
            std::cout << "Write Acquired: " << write_acquired << "\n";
            std::cout << "Notifying: " << notifying << "\n";
            std::cout << "Flag Notify: " << flag_notify << "\n";
            std::cout << "Flag Read: " << flag_read << "\n";
            std::cout << "Flag Write: " << flag_write << "\n";
            std::cout << "Flag Indicate: " << flag_indicate << "\n";
        }
    };

    struct Measurement {
        std::chrono::year_month_day date{};
        std::chrono::hh_mm_ss<std::chrono::seconds> time;
        float value{};
        std::uint8_t meal_ctx{};
    };


    GlucoseDevice selected_device{};
    std::vector<GlucoseDevice> glucose_devices{};

    std::map<int, Measurement> measurements{};

    Characteristic racp_characteristic{};
    Characteristic glucose_measurement_characteristic{};
    Characteristic glucose_measurement_context_characteristic{};

    std::mutex writing_mutex;
    std::condition_variable writing_cv;
    bool racp_writing_completed = false;
    bool pairing_timed_out = false;

    std::mutex connect_mutex;
    std::condition_variable connect_cv;

    // Global DBUS objects
    std::shared_ptr<DBus::StandaloneDispatcher> dispatcher;
    std::shared_ptr<DBus::Connection> system_bus_connection;
    std::shared_ptr<DBus::ObjectProxy> bluez_root_op;
    std::shared_ptr<DBus::ObjectProxy> adapter_op;

    enum MeasurementUnit {
        MMOL_PER_L, // millimoles per liter
        MG_PER_DL // milligrams per deciliter
    };

    MeasurementUnit output_measurement_unit = MMOL_PER_L;

    bool debug_messages = false;

    uint8_t scan_interval_secs = 5; // s
    uint16_t user_input_timeout_ms = 20000; // ms

    std::string convert_meal_context_to_string(const uint8_t meal_context) {
        switch (meal_context) {
            case 1: return "Preprandial";
            case 2: return "Postprandial";
            case 3: return "Fasting";
            case 4: return "Casual";
            case 5: return "Bedtime";
            default: return "Unknown";
        }
    }


    void write_measurements_to_file() {
        const std::string output_filename = "glucose_measurements.csv";
        std::filesystem::remove(output_filename);
        std::ofstream outFile(output_filename);

        if (!outFile.is_open()) {
            std::cerr << "Error: Could not open the file.\n";
            return;
        }

        // Header
        outFile << "timestamp,measured_value_mmol_per_l,meal_context\n";

        for (const auto &[date, time, value, meal_ctx]: measurements | std::views::values) {
            outFile << std::format("{:%Y-%m-%d}", date) << " " << std::format("{:%H:%M:%S}", time) << "," <<
                    std::format("{:.1f}", value) << ",";
            if (meal_ctx != 0) {
                outFile << convert_meal_context_to_string(meal_ctx);
            }
            outFile << "\n";
        }

        outFile.close();

        std::cout << "Wrote measurements to file: " << output_filename << "\n";
    }


    float convert_med_float_16(const uint8_t hi, const uint8_t lo, const int add_to_exp) {
        const auto exp = hi >> 7 == 1 ? -((static_cast<uint8_t>(~hi) >> 4) + 1) : hi >> 4;
        const auto mantissa = lo | ((hi & 0x0f) << 8);

        const auto value = static_cast<float>(mantissa * std::pow(10, exp + add_to_exp));

        return value;
    }


    std::tuple<std::uint16_t, Measurement> get_measurement(const std::vector<uint8_t> &value) {
        const auto flags = value[0];
        const bool time_offset_field_present = (flags & (1 << 0)) != 0;
        //bool glucose_concentration_and_type_sample_location_present = ((flags & (1 << 1)) != 0);
        const bool glucose_concentration_in_units_of_mol_per_l = (flags & (1 << 2)) != 0;
        //bool sensor_status_annunciation_field_present = ((flags & (1 << 3)) != 0);
        //bool record_includes_context_information = ((flags & (1 << 4)) != 0);

        auto seq_no = static_cast<std::uint16_t>(value[1] | (value[2] << 8));

        const auto year = static_cast<std::uint16_t>(value[3] | (value[4] << 8));
        const auto month = static_cast<std::uint8_t>(value[5]);
        const auto day = static_cast<std::uint8_t>(value[6]);
        const auto hour = static_cast<std::uint8_t>(value[7]);
        const auto minute = static_cast<std::uint8_t>(value[8]);
        const auto second = static_cast<std::uint8_t>(value[9]);

        const auto ymd = std::chrono::year(year) / std::chrono::month(month) / std::chrono::day(day);
        const std::chrono::hh_mm_ss hms{
            std::chrono::hours(hour) + std::chrono::minutes(minute) + std::chrono::seconds(second)
        };

        auto time_offset = 0;
        if (time_offset_field_present) {
            time_offset = static_cast<std::uint16_t>(value[10] | (value[11] << 8));
            //std::cout << "[DBUS message] -> Time offset: " << time_offset << "\n";
        }

        const auto tp = std::chrono::sys_days(ymd) + hms.to_duration() + std::chrono::minutes(time_offset);
        const auto dp = floor<std::chrono::days>(tp);
        const std::chrono::year_month_day new_ymd{dp};
        const std::chrono::hh_mm_ss new_tod{tp - dp};

        auto inc_exp = 3;
        if (!glucose_concentration_in_units_of_mol_per_l) {
            inc_exp = 5;
        }

        float measured_value = time_offset_field_present
                                   ? convert_med_float_16(value[13], value[12], inc_exp)
                                   : convert_med_float_16(value[11], value[10], inc_exp);
        if (!glucose_concentration_in_units_of_mol_per_l) {
            measured_value /= 18.016f;
        }

        //std::cout << "Measured value: " << measured_value << "\n";

        // if (glucose_concentration_and_type_sample_location_present) {
        //     auto type_sample_location = time_offset_field_present ? value[14] : value[12];
        //     auto type = type_sample_location >> 4;
        //     auto sample_location = type_sample_location & 0x0F;
        //     //std::cout << "-> Type: " << type << "\n";
        //     //std::cout << "-> Sample location: " << sample_location << "\n";
        // }

        //std::cout << "Unit is mol/L: " << std::boolalpha << glucose_concentration_in_units_of_mol_per_l << std::noboolalpha
        //            << '\n';


        Measurement measurement{};

        measurement.date = new_ymd;
        measurement.time = new_tod;
        measurement.value = measured_value;

        auto tuple = std::make_tuple(seq_no, measurement);
        return tuple;
    }


    GlucoseDevice create_device_from_properties(
        const std::map<std::string, std::map<std::string, DBus::Variant> >::mapped_type &props) {
        auto device = GlucoseDevice{};
        device.address = props.at("Address").to_string();
        device.address_type = props.at("AddressType").to_string();
        device.name = props.at("Name").to_string();
        device.connected = props.at("Connected").to_bool();
        if (props.contains("Appearance")) {
            device.appearance = props.at("Appearance").to_uint16();
        }
        if (props.contains("Class")) {
            device.class_of_device = props.at("Class").to_uint32();
        }
        device.bonded = props.at("Bonded").to_bool();
        device.paired = props.at("Paired").to_bool();
        device.trusted = props.at("Trusted").to_bool();
        device.services_resolved = props.at("ServicesResolved").to_bool();

        if (props.contains("AdvertisingFlags")) {
            auto data = props.at("AdvertisingFlags");
            const auto vflags = data.to_vector<uint8_t>();
            //std::cout << "[DBUS message] -> AdvertisingFlags: size = " << vflags.size() << "\n";
            if (debug_messages) {
                std::cout << " -> AdvertisingFlags: ";
                for (const auto flag_char: vflags) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<unsigned int>(flag_char) << " ";
                }
            }
            std::cout << std::dec << std::endl;
            const auto flags = vflags[0];
            const bool discoverable = flags & 0x01 || flags & 0x02;
            device.discoverable = discoverable;
        }
        return device;
    }
}


static void on_properties_changed_when_scanning(const std::vector<uint8_t> &new_flags, const DBus::Path &path) {
    if (new_flags.empty()) {
        return;
    }

    if (debug_messages) {
        std::cout << "on_properties_changed_when_scanning: path = " << path << "\n";
        std::cout << "on_properties_changed_when_scanning: flags = " << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(new_flags[0]) << std::dec << std::endl;
    }

    const auto it = std::ranges::find_if(glucose_devices,
                                         [&](const GlucoseDevice &d) { return d.path == path; });

    if (it != glucose_devices.end()) {
        const bool discoverable = new_flags[0] & 0x01 || new_flags[0] & 0x02;
        it->discoverable = discoverable;
    }
}


static void on_connected_changed_when_connecting(const bool connected, const DBus::Path &path) {
    if (debug_messages) {
        std::cout << "on_properties_changed_when_connecting: path = " << path << "\n";
        std::cout << "on_properties_changed_when_connecting: connected = " << std::boolalpha << connected <<
                std::noboolalpha << "\n";
    }

    if (selected_device.path == path) {
        {
            std::lock_guard lock(connect_mutex);
            selected_device.connected = connected;
        }
        connect_cv.notify_all();
    } else {
        if (debug_messages) {
            std::cout << "WTF" << '\n';
        }
    }
}


static void on_services_resolved_changed_when_connecting(const bool resolved, const DBus::Path &path) {
    if (debug_messages) {
        std::cout << "on_services_resolved_changed_when_connecting: path = " << path << "\n";
        std::cout << "on_services_resolved_changed_when_connecting: resolved = " << std::boolalpha << resolved <<
                std::noboolalpha << "\n";
    }

    if (selected_device.path == path) {
        {
            std::lock_guard lock(connect_mutex);
            selected_device.services_resolved = resolved;
        }
        connect_cv.notify_all();
    } else {
        if (debug_messages) {
            std::cout << "WTF" << '\n';
        }
    }
}


static void on_properties_changed_when_pairing(const std::string &interface,
                                               PropertyMap changed_props,
                                               [[maybe_unused]] const std::vector<std::string> &invalidated_props,
                                               const DBus::Path &path) {
    if (debug_messages) {
        std::cout << "on_properties_changed_during_pairing: path = " << path << "\n";
    }
    for (const auto &key: std::views::keys(changed_props)) {
        if (debug_messages) {
            std::cout << "Key: " << key << '\n';
        }
    }

    if (interface == "org.bluez.Device1") {
        if (changed_props.contains("Paired")) {
            const bool paired = changed_props.at("Paired").to_bool();
            if (debug_messages) {
                std::cout << "on_properties_changed_during_pairing() -> Device paired: " << std::boolalpha << paired <<
                        std::noboolalpha << "\n";
            }
            selected_device.paired = paired;
        }

        if (changed_props.contains("Bonded")) {
            const bool bonded = changed_props.at("Bonded").to_bool();
            if (debug_messages) {
                std::cout << "on_properties_changed_during_pairing() -> Device bonded: " << std::boolalpha << bonded <<
                        std::noboolalpha << "\n";
            }
            selected_device.bonded = bonded;
        }

        if (changed_props.contains("Connected")) {
            const bool connected = changed_props.at("Connected").to_bool();
            if (debug_messages) {
                std::cout << "on_properties_changed_during_pairing() -> Device Connected: " << std::boolalpha <<
                        connected
                        << std::noboolalpha << "\n";
            }
            selected_device.connected = connected;
        }

        if (changed_props.contains("ServicesResolved")) {
            const bool resolved = changed_props.at("ServicesResolved").to_bool();
            if (debug_messages) {
                std::cout << "on_properties_changed_during_pairing() -> Services Resolved:" << std::boolalpha <<
                        resolved <<
                        std::noboolalpha << "\n";
            }
            selected_device.services_resolved = resolved;
        }
    }
}


static void on_properties_changed_for_glucose_measurement(const std::string &interface,
                                                          PropertyMap changed_props,
                                                          [[maybe_unused]] const std::vector<std::string> &
                                                          invalidated_props,
                                                          [[maybe_unused]] const DBus::Path &sender_path) {
    //std::cout << "on_properties_changed_for_glucose_measurement: " << "\n";
    //std::cout << " -> PropertiesChanged: " << interface << "\n";

    if (interface == "org.bluez.GattCharacteristic1") {
        //std::cout << "[DBUS message] -> GattCharacteristic1 Props changed: " << changed_props.size() << "\n";
        //std::cout << "Sender path: " << sender_path << "\n";
        if (changed_props.contains("Notifying")) {
            if (debug_messages) {
                std::cout << " -> Notifying: " << std::boolalpha << changed_props.at("Notifying").to_bool() <<
                        std::noboolalpha << "\n";
            }
        }
        if (changed_props.contains("Value")) {
            const std::vector<uint8_t> value = changed_props.at("Value").to_vector<uint8_t>();
            std::cout << "." << std::flush;
            // std::cout << " -> Value: ";
            // for (const auto i: value) {
            //     std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(i) <<
            //             std::dec << " ";
            // }
            // std::cout << "\n";
            const auto measurement = get_measurement(value);
            auto key = std::get<0>(measurement);
            auto mval = std::get<1>(measurement);
            // std::cout << "Measurement: key = " << std::get<0>(measurement) << "\n";
            // std::cout << "Measurement: value = " << std::get<1>(measurement).value << "\n";
            // std::cout << "Measurement: date = " << std::format("{:%d-%m-%Y}", std::get<1>(measurement).date) << "\n";
            // std::cout << "Measurement: time = " << std::format("{:%H:%M:%S}", std::get<1>(measurement).time) << "\n";

            // Update if measurement entry exists for key
            if (measurements.contains(key)) {
                auto old_measurement = measurements[key];
                old_measurement.date = mval.date;
                old_measurement.time = mval.time;
                old_measurement.value = mval.value;
                measurements[key] = old_measurement;
            } else {
                measurements.insert({key, mval});
            }
        }
    }
}


static void on_properties_changed_for_racp([[maybe_unused]] const std::string &interface,
                                           PropertyMap changed_props,
                                           [[maybe_unused]] const std::vector<std::string> &invalidated_props,
                                           [[maybe_unused]] const DBus::Path &sender_path) {
    if (debug_messages) {
        std::cout << "\non_properties_changed_for_racp():\n";
    }
    if (changed_props.contains("Notifying")) {
        if (debug_messages) {
            std::cout << " -> Notifying: " << std::boolalpha << changed_props.at("Notifying").to_bool() <<
                    std::noboolalpha
                    << "\n";
        }
    }
    if (changed_props.contains("Value")) {
        const std::vector<uint8_t> value = changed_props.at("Value").to_vector<uint8_t>();
        if (debug_messages) {
            std::cout << " -> Value: ";
        }
        for (const auto i: value) {
            if (debug_messages) {
                std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(i) <<
                        std::dec << " ";
            }
        }
        std::cout << "\n";
        {
            std::lock_guard lock(writing_mutex);
            racp_writing_completed = true;
        }

        writing_cv.notify_one();
    }
}


static void on_properties_changed_for_glucose_measurement_context([[maybe_unused]] const std::string &interface,
                                                                  PropertyMap changed_props,
                                                                  [[maybe_unused]] const std::vector<std::string> &
                                                                  invalidated_props,
                                                                  [[maybe_unused]] const DBus::Path &sender_path) {
    //std::cout << "on_properties_changed_for_glucose_measurement_context: " << "\n";

    if (changed_props.contains("Notifying")) {
        if (debug_messages) {
            std::cout << " -> Notifying: " << std::boolalpha << changed_props.at("Notifying").to_bool() <<
                    std::noboolalpha
                    << "\n";
        }
    }

    if (changed_props.contains("Value")) {
        const std::vector<uint8_t> value = changed_props.at("Value").to_vector<uint8_t>();
        //std::cout << " -> Value: ";
        // for (auto i: value) {
        //     std::cout << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(i) <<
        //             std::dec << " ";
        // }
        // std::cout << "\n";

        //auto flags = value[0];
        auto seqno = static_cast<std::uint16_t>(value[1] | (value[2] << 8));
        const auto ctx = value[3];

        // std::cout << "flags: " << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(
        //     flags) << std::dec << "\n";
        // std::cout << "seqno: " << seqno << "\n";
        // std::cout << "ctx: " << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(ctx)
        //         << std::dec << "\n";

        // Update if measurement entry exists for key
        if (measurements.contains(seqno)) {
            auto old_measurement = measurements[seqno];
            old_measurement.meal_ctx = ctx;
            measurements[seqno] = old_measurement;
        } else {
            Measurement mval{.meal_ctx = ctx};
            measurements.insert({seqno, mval});
        }
    }
}


static void add_glucose_device(DBus::Path path, InterfaceMap interfaces) {
    if (interfaces.contains("org.bluez.Device1")) {
        if (const auto &props = interfaces.at("org.bluez.Device1"); props.contains("UUIDs")) {
            for (DBus::Variant uuids = props.at("UUIDs"); const auto &uuid: uuids.to_vector<std::string>()) {
                if (uuid == GLUCOSE_SERVICE_UUID) {
                    //std::cout << "Found device: " << path << '\n';
                    // for (const auto &[key, value]: props) {
                    //     std::cout << "Key: " << key << '\n';
                    // }
                    // check to see if the device is already discovered
                    auto it = std::ranges::find_if(glucose_devices,
                                                   [&](const GlucoseDevice &d) { return d.path == path; });

                    if (it == glucose_devices.end()) {
                        if (debug_messages) {
                            std::cout << "Registering new device: " << path << '\n';
                        }
                        GlucoseDevice device = create_device_from_properties(props);
                        device.path = path;


                        // Add listener for updates to AdvertisingFlags
                        if (debug_messages) {
                            std::cout << "Adding PropertiesChanged listener for AdvertisingFlags" << '\n';
                        }
                        auto device_op = system_bus_connection->create_object_proxy("org.bluez", path);
                        auto adv_flags_pp = device_op->create_property<std::vector<uint8_t> >(
                            "org.bluez.Device1",
                            "AdvertisingFlags"
                        );
                        auto adv_flags_connection = adv_flags_pp->signal_property_changed().connect(
                            sigc::bind(sigc::ptr_fun(&on_properties_changed_when_scanning), path));

                        device.op = device_op;
                        device.pp = adv_flags_pp;
                        device.pp_conn = adv_flags_connection;
                        glucose_devices.push_back(device);
                    } else {
                        std::cerr << "Need tp update device" << std::endl;
                    }
                }
            }
        }
    }
}

static void on_interfaces_added(const DBus::Path &path, const InterfaceMap &interfaces) {
    //std::cout << "on_interfaces_added: path = " << path << "\n";
    add_glucose_device(path, interfaces);
}


static std::uint32_t get_passkey() {
    std::cout << "Enter passkey (6 digits): " << std::flush;

    pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};

    // Wait up to 15000 ms (15 seconds) for input to be available
    const int ret = poll(&pfd, 1, user_input_timeout_ms);

    if (ret > 0 && pfd.revents & POLLIN) {
        if (std::string passkey; std::getline(std::cin, passkey)) {
            try {
                const auto ret_pk = static_cast<std::uint32_t>(std::stoul(passkey));
                std::cout << "Provided passkey: " << ret_pk << "\n" << std::flush;
                return ret_pk;
            } catch (const std::exception &e) {
                std::cerr << "Invalid number format: " << e.what() << "\n";
                throw DBus::Error("org.bluez.Error.Rejected", "error");
            }
        }
        std::cout << "getline error" << '\n';
        throw DBus::Error("org.bluez.Error.Rejected", "error");
    }
    if (ret == 0) {
        std::cout << "No passkey entered in time\n";
        pairing_timed_out = true;
        throw DBus::Error("org.bluez.Error.Rejected", "timed out");
    }
    std::cout << "Unknown error waiting for passkey input\n";
    throw DBus::Error("org.bluez.Error.Rejected", "error");

    return 0;
}


static std::uint32_t on_request_passkey(const DBus::Path &device) {
    std::cout << "Received request for pass key for device: " << device << "\n";

    return get_passkey();
}

/*
void on_request_confirmation(DBus::Path device, uint32_t passkey) {
    std::cout << "Confirming passkey " << passkey << " for device: " << device << "\n";
}

void on_authorize_service(DBus::Path device, std::string uuid) {
    std::cout << "Authorizing service " << uuid << " for device: " << device << "\n";
}
*/

static void find_managed_glucose_devices(ManagedObjectsMap result) {
    for (auto &[path, interfaces]: result) {
        add_glucose_device(path, interfaces);
    }
}

static void list_devices() {
    int count = 1;
    for (const auto &device: glucose_devices) {
        std::cout << "[" << count << "] " << device.name << "\n";
        count++;
    }
}

static bool get_selection() {
    std::cout << "Enter device number: " << std::flush;

    pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};


    // Wait up to 15000 ms (15 seconds) for input to be available
    const int ret = poll(&pfd, 1, user_input_timeout_ms);

    if (ret > 0 && pfd.revents & POLLIN) {
        if (std::string dev_number; std::getline(std::cin, dev_number)) {
            try {
                if (debug_messages) {
                    std::cout << std::format("\nSelected: {}\n", glucose_devices[std::stoi(dev_number) - 1].name);
                }
                selected_device = glucose_devices[std::stoi(dev_number) - 1];
                return true;
            } catch (const std::exception &e) {
                std::cerr << "\nInvalid number format: " << e.what() << "\n";
                return false;
            }
        }
        std::cout << "\ngetline error" << '\n';
        return false;
    }
    if (ret == 0) {
        std::cout << "\nNo device number entered in time\n";
        pairing_timed_out = true;
        return false;
    }

    std::cout << "\nUnknown error waiting for passkey input\n";

    return false;
}

/*
static void print_characteristic_properties(const PropertyMap &props) {
    std::cout << "Characteristic Properties:\n";

    // Bind variant by const reference, then make a lightweight non-const copy
    for (const auto &[key, const_variant]: props) {
        DBus::Variant variant = const_variant;
        std::cout << "  " << key << ": ";

        try {
            if (key == "UUID") {
                std::cout << variant.to_string();
            } else if (key == "Service") {
                std::cout << variant.to_path();
            } else if (key == "Notifying" || key == "WriteAcquired" || key == "NotifyAcquired") {
                std::cout << (variant.to_bool() ? "true" : "false");
            } else if (key == "MTU") {
                std::cout << variant.to_uint16() << " bytes";
            } else if (key == "Flags") {
                auto flags = variant.to_vector<std::string>();
                std::cout << "[";
                for (size_t i = 0; i < flags.size(); ++i) {
                    std::cout << flags[i] << (i + 1 < flags.size() ? ", " : "");
                }
                std::cout << "]";
            } else if (key == "Value") {
                auto bytes = variant.to_vector<uint8_t>();
                std::cout << "[";
                for (size_t i = 0; i < bytes.size(); ++i) {
                    std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(bytes[i])
                            << (i + 1 < bytes.size() ? " " : "");
                }
                std::cout << std::dec << "] (" << bytes.size() << " bytes)";
            } else {
                std::cout << "<Variant signature: " << variant.signature() << ">";
            }
        } catch (const std::exception &e) {
            std::cout << "<Error unpacking: " << e.what() << ">";
        }

        std::cout << "\n";
    }
}
*/
static Characteristic get_characteristic(const std::string &path, const PropertyMap &props) {
    //    std::cout << "get_characteristic: " << path << "\n";

    Characteristic characteristic{.path = path};

    //    std::cout << "get_characteristic: props.size() = " << props.size() << "\n";

    // Bind variant by const reference, then make a lightweight non-const copy
    for (const auto &[key, const_variant]: props) {
        DBus::Variant variant = const_variant;
        //        std::cout << "key: " << key << "\n";
        try {
            if (key == "UUID") {
                characteristic.uuid = variant.to_string();
            } else if (key == "Notifying") {
                characteristic.notifying = variant.to_bool();
            } else if (key == "WriteAcquired") {
                characteristic.write_acquired = variant.to_bool();
            } else if (key == "NotifyAcquired") {
                characteristic.notify_acquired = variant.to_bool();
            } else if (key == "Flags") {
                for (auto flags = variant.to_vector<std::string>(); const auto &flag: flags) {
                    if (flag == "read") {
                        characteristic.flag_read = true;
                    } else if (flag == "write") {
                        characteristic.flag_write = true;
                    } else if (flag == "notify") {
                        characteristic.flag_notify = true;
                    } else if (flag == "indicate") {
                        characteristic.flag_indicate = true;
                    }
                }
            }
        } catch (const std::exception &e) {
            std::cout << "<Error unpacking: " << e.what() << ">";
        }
    }

    return characteristic;
}


static void check_gatt_services(const std::string &device_path, const ManagedObjectsMap &map) {
    for (auto &[path, interfaces]: map) {
        if (path.rfind(device_path + "/", 0) != 0) {
            continue;
        }

        if (auto service_it = interfaces.find("org.bluez.GattService1"); service_it != interfaces.end()) {
            const PropertyMap &property_map = service_it->second;
            //bool primary = property_map.at("Primary").to_bool();

            // std::cout << "Found Service at: " << path << "\n"
            //         << "  UUID:    " << uuid << "\n"
            //         << "  Primary: " << (primary ? "true" : "false") << "\n";

            if (std::string uuid = property_map.at("UUID").to_string(); uuid == GLUCOSE_SERVICE_UUID) {
                // std::cout << "Glucose service found at path: " << path << "\n";
                auto service_path = path;

                for (auto &[cpath, cinterfaces]: map) {
                    if (cpath.rfind(service_path + "/", 0) != 0) {
                        continue;
                    }
                    if (auto characteristic_it = cinterfaces.find("org.bluez.GattCharacteristic1");
                        characteristic_it != cinterfaces.end()) {
                        const PropertyMap &characteristic_property_map = characteristic_it->second;
                        std::string characteristic_uuid = characteristic_property_map.at("UUID").to_string();
                        // std::cout << "Found Characteristic at: " << path << "\n" << "  UUID:    " << uuid << "\n";
                        // print_characteristic_properties(property_map);
                        if (characteristic_uuid == RECORD_ACCESS_CONTROL_POINT_CHARACTERISTIC_UUID) {
                            racp_characteristic = get_characteristic(cpath, characteristic_property_map);
                        }
                        if (characteristic_uuid == GLUCOSE_MEASUREMENT_CHARACTERISTIC_UUID) {
                            glucose_measurement_characteristic = get_characteristic(cpath, characteristic_property_map);
                        }
                        if (characteristic_uuid == GLUCOSE_MEASUREMENT_CONTEXT_CHARACTERISTIC_UUID) {
                            glucose_measurement_context_characteristic = get_characteristic(
                                cpath, characteristic_property_map);
                        }
                    }
                }
            }
        }
    }
}


static void init() {
    // Create a dispatcher
    dispatcher = DBus::StandaloneDispatcher::create();

    if (debug_messages) {
        std::cout << "Dispatcher created." << '\n';
    }

    // Get connection to system bus
    system_bus_connection = dispatcher->create_connection(DBus::BusType::SYSTEM);
    if (debug_messages) {
        std::cout << "Connected to system bus." << '\n';
    }

    // Create proxy to BlueZ root path
    bluez_root_op = system_bus_connection->create_object_proxy("org.bluez", "/");
    if (debug_messages) {
        std::cout << "Root object proxy created.\n";
    }

    // Create proxy to adapter
    adapter_op = system_bus_connection->create_object_proxy("org.bluez", "/org/bluez/hci0");
    if (debug_messages) {
        std::cout << "Adapter object proxy created.\n";
    }
}


static void remove_device_from_cache() {
    const auto remove_device_mp = adapter_op->create_method<void(DBus::Path)>(
        "org.bluez.Adapter1",
        "RemoveDevice"
    );
    try {
        (*remove_device_mp)(selected_device.path);
        std::cout << "Discovered device removed from cache\n";
    } catch (const DBus::Error &e) {
        std::cerr << "RemoveDevice failed: " << e.what() << "\n";
    }
}


void print_help(std::string app_name) {
    std::cout << "Usage: " << app_name << " [OPTION]...\n\
Read measurements from glucose meter device \n\
\n\
  -d, --debug                  display debug messages\n\
  -u, --unit=UNIT              choose output measurement unit. UNIT types are [mmol] (for mmol per liter; default unit) or [mg] (mg per dL)\n\
  -s, --scan-interval=SECONDS  scan interval (in seconds). Default is 5 seconds.\n\
\n";
}

bool parse_options(const char **argv) {
    argh::parser cmdl({"-u", "--unit", "-f", "-s", "--scan-interval"});
    cmdl.parse(argv);

    std::filesystem::path fullPath = cmdl[0];
    auto app_name = fullPath.filename().string();

    debug_messages = (cmdl[{"d", "debug", "v", "verbose"}]);

    if (debug_messages) {
        std::cout << "Debug mode is on\n";
    }

    if (cmdl[{"h", "help"}]) {
        print_help(app_name);
        return false;
    }


    std::string provided_unit = cmdl({"u", "unit"}).str();
    if (!provided_unit.empty())
    {
        if (provided_unit == "mmol") {
            if (debug_messages) {
                std::cout << "Provided unit = mmol/L (EU)\n";
            }
            output_measurement_unit = MeasurementUnit::MMOL_PER_L;
        } else if (provided_unit == "mg") {
            if (debug_messages) {
                std::cout << "Provided unit = mg/dL (US)\n";
            }
            output_measurement_unit = MeasurementUnit::MG_PER_DL;
        } else {
            std::cerr << "Invalid value for unit: " << provided_unit << '\n';
            return false;
        }
    }

    std::string scan_interval_str = cmdl({"s", "scan-interval"}).str();

    if (!scan_interval_str.empty()) {
        try {
            auto s_interval = static_cast<uint8_t>(stoi(scan_interval_str));
            scan_interval_secs = s_interval;
            std::cout << "Scan interval: " << static_cast<int>(scan_interval_secs) << '\n';
        } catch (const std::exception &e) {
            std::cerr << "Invalid value for scan interval: " << scan_interval_str << '\n';
            return false;
        }
    }
    
    return true;
}

int main(int argc, const char *argv[]) {
    if (!parse_options(argv)) {
        return EXIT_FAILURE;
    }

    //return EXIT_SUCCESS;

    try {
        init();

        // Get managed devices
        if (debug_messages) {
            std::cout << "Retrieving managed objects." << '\n';
        }

        auto managed_objects_result = bluez_root_op->getObjectManagerInterface()->GetManagedObjects();
        if (debug_messages) {
            std::cout << "Managed objects retrieved.\n";
        }

        // Retrieve glucose devices from managed objects
        if (debug_messages) {
            std::cout << "Find managed glucose service devices." << '\n';
        }
        find_managed_glucose_devices(managed_objects_result);

        if (debug_messages) {
            if (glucose_devices.empty()) {
                std::cout << "No glucose service devices found in managed objects.\n";
            } else {
                std::cout << "Found " << glucose_devices.size() << " glucose service devices in managed objects.\n";
            }
        }

        // Scan for devices

        // Set up BLE discovery listeners

        // Create SignalProxy for InterfacesAdded signals
        auto interfaces_added_sp = bluez_root_op->getObjectManagerInterface()->create_signal<void(
            DBus::Path, InterfaceMap)>("InterfacesAdded");

        // Attach function handler
        auto interface_added_conn = interfaces_added_sp->connect(sigc::ptr_fun(&on_interfaces_added));
        if (debug_messages) {
            std::cout << "Registered listener for InterfacesAdded.\n";
        }


        // Define the hardware discovery filter parameters

        // BlueZ SetDiscoveryFilter expects an "a{sv}" (std::map<std::string, DBus::Variant>)
        PropertyMap discovery_filter;

        // Add the standard Glucose Service UUID (0x1808) to filter
        std::vector<std::string> uuids = {GLUCOSE_SERVICE_UUID};
        DBus::Variant uuid_filter(uuids);
        discovery_filter["UUIDs"] = uuid_filter;

        // Restrict the physical radio scan to Low Energy (LE) only
        discovery_filter["Transport"] = DBus::Variant(std::string("le"));

        // Create SetDiscoveryFilter method proxy on adapter interface
        auto set_filter_mp = adapter_op->create_method<void(PropertyMap)>(
            "org.bluez.Adapter1",
            "SetDiscoveryFilter");

        if (debug_messages) {
            std::cout << "Applying Glucose Profile UUID discovery filter (0x1808)\n";
        }

        // Call the set filter proxy
        (*set_filter_mp)(discovery_filter);

        // Create start_discovery method proxy on adapter interface
        auto start_discovery_mp = adapter_op->create_method<void()>(
            "org.bluez.Adapter1",
            "StartDiscovery");
        if (debug_messages) {
            std::cout << "StartDiscovery method created.\n";
        }

        // Create stop_discovery method proxy on adapter interface
        auto stop_discovery_mp = adapter_op->create_method<void()>(
            "org.bluez.Adapter1",
            "StopDiscovery");
        if (debug_messages) {
            std::cout << "StopDiscovery method created.\n";
        }

        // Call the start discover proxy
        if (debug_messages) {
            std::cout << "Requesting BLE scan from bluetoothd\n";
        }
        (*start_discovery_mp)();

        // Sleep N seconds
        std::cout << "Scanning for " << static_cast<int>(scan_interval_secs) << " seconds...\n";
        std::this_thread::sleep_for(std::chrono::seconds(scan_interval_secs));

        // Call the stop discover proxy
        (*stop_discovery_mp)();
        if (debug_messages) {
            std::cout << "Stopping BLE scan\n";
        }

        // Clean up callback for InterfacesAdded
        if (auto signal_removed = bluez_root_op->getObjectManagerInterface()->remove_signal(interfaces_added_sp); !
            signal_removed) {
            std::cerr << "Failed to remove interfaces added signal.\n";
        }
        interface_added_conn.disconnect();
        interfaces_added_sp.reset();

        if (debug_messages) {
            std::cout << "Deregistered listener for InterfacesAdded.\n";
            std::cout << "Discovery ended.\n";
        }

        std::cout << "Glucose devices found: " << glucose_devices.size() << "\n";


        // Exit if no devices found
        if (glucose_devices.empty()) {
            std::cout << "No glucose devices found. Exiting.\n";
            return EXIT_SUCCESS;
        }

        // Ask user to select device if more than one is found
        // Exit if user selects invalid device
        // Select device if only one exists
        if (glucose_devices.size() > 1) {
            std::cout << "More than one glucose device found. Please select one.\n";
            list_devices();
            if (!get_selection()) {
                std::cout << "Invalid selection. Exiting.\n";
                return EXIT_FAILURE;
            }
        } else {
            selected_device = glucose_devices[0];
        }

        std::cout << "Selected device: " << selected_device.name << "\n";


        // Clean up callback handler for AdvertisingFlags
        if (debug_messages) {
            std::cout << "Deregistering listener for AdvertisingFlags" << '\n';
        }
        selected_device.pp->signal_property_changed().clear();
        selected_device.pp_conn.disconnect();
        selected_device.pp.reset();


        // Selected device must be discoverable if it is not bonded
        if (!selected_device.bonded && !selected_device.discoverable) {
            // Remove device
            remove_device_from_cache();

            std::cout << "Selected device is not discoverable. Set device to pairing mode and try again.\n";
            return EXIT_SUCCESS;
        }

        // If it is not bonded, initiate pairing
        if (!selected_device.bonded || !selected_device.paired) {
            std::cout << "Device is not bonded. Pairing required.\n";


            // Handle pairing
            if (debug_messages) {
                std::cout << "Handling pairing.\n";
            }

            // Create agent to handle pairing, as local object
            std::string agent_path = "/temp_agent/glucose_device_authenticator";
            auto pairing_agent = system_bus_connection->create_object(agent_path);
            if (debug_messages) {
                std::cout << "Created local pairing agent\n";
            }

            // Create callback handler for RequestPasskey
            auto passkey_requested_method = pairing_agent->create_method<std::uint32_t(DBus::Path)>(
                "org.bluez.Agent1",
                "RequestPasskey",
                sigc::ptr_fun(&on_request_passkey)
            );
            if (debug_messages) {
                std::cout << "Pairing method created on local agent with callback listener function\n";
            }

            // Create bluez manager proxy for registering local agent and default agent for passkey
            auto bluez_manager_op = system_bus_connection->create_object_proxy("org.bluez", "/org/bluez");

            // Create register agent, to register pairing agent, as proxy object
            auto register_mp = bluez_manager_op->create_method<void(DBus::Path, std::string)>(
                "org.bluez.AgentManager1",
                "RegisterAgent");

            // Register agent
            (*register_mp)(pairing_agent->path(), "KeyboardOnly");
            if (debug_messages) {
                std::cout << "Registered pairing agent\n";
            }

            // Create request default agent, to register agent as default handler, as proxy object
            auto request_default_agent_mp = bluez_manager_op->create_method<void(DBus::Path)>(
                "org.bluez.AgentManager1",
                "RequestDefaultAgent");

            // Set agent as default handler
            (*request_default_agent_mp)(pairing_agent->path());
            if (debug_messages) {
                std::cout << "Registered pairing agent as default agent\n";
            }

            // Create Pair method proxy
            auto pair_method_proxy = selected_device.op->create_method<void()>("org.bluez.Device1", "Pair");

            // Add callback listener for PropertiesChanged
            auto device_props_signal = selected_device.op->create_signal<PropertiesChangedSignature>(
                "org.freedesktop.DBus.Properties",
                "PropertiesChanged"
            );

            auto properties_changed_conn = device_props_signal->connect(
                sigc::bind(sigc::ptr_fun(&on_properties_changed_when_pairing), selected_device.path));

            if (debug_messages) {
                std::cout << "Registered callback listener for PropertiesChanged\n";
            }

            // Initiate pairing
            try {
                (*pair_method_proxy)();
                std::cout << "Pairing completed\n";
            } catch (const DBus::ErrorNoReply &e) {
                // Handle timeout (e.g., user took too long to enter PIN)
                std::cerr << "Error NoReply: " << e.what() << "\n";
                //return EXIT_FAILURE;
                // Remove device
                remove_device_from_cache();

                std::abort();
            } catch (const DBus::Error &e) {
                if (pairing_timed_out) {
                    std::cerr << "Pairing error: timed out" << std::endl;
                } else {
                    std::cerr << "Pairing error: name: " << e.name() << ", what: " << e.what() << std::endl;
                }
                // Remove device
                remove_device_from_cache();

                // Clean up callback handler for pairing and properties changed?
                return EXIT_FAILURE;
            }


            // Clean up properties changed callback
            properties_changed_conn.disconnect();
            device_props_signal.reset();

            // Clean up callback handler for pairing?
        } else {
            if (debug_messages) {
                std::cout << "Device is already bonded\n";
            }
        }


        // Connect to selected device, if not connected or serices are not resolved
        if (!selected_device.connected || !selected_device.services_resolved) {
            std::cout << "Connecting to selected device.\n";
            auto connect_method_proxy = selected_device.op->create_method<void()>("org.bluez.Device1", "Connect");
            auto connect_call = connect_method_proxy->create_call_message();

            // Add property change handler for Connected and ServicesResolved
            selected_device.connected_pp = selected_device.op->create_property<bool>(
                "org.bluez.Device1",
                "Connected"
            );
            selected_device.connected_pp->signal_property_changed().connect(
                sigc::bind(sigc::ptr_fun(&on_connected_changed_when_connecting), selected_device.path));


            selected_device.services_resolved_pp = selected_device.op->create_property<bool>(
                "org.bluez.Device1",
                "ServicesResolved"
            );
            selected_device.services_resolved_pp->signal_property_changed().connect(
                sigc::bind(sigc::ptr_fun(&on_services_resolved_changed_when_connecting), selected_device.path));


            // Blocking call, wait for max 5 second
            try {
                auto return_message = connect_method_proxy->call(connect_call, 5000);
            } catch ([[maybe_unused]] const DBus::Error &e) {
                std::cout << "Device connection failure. Is device turned on?" << std::endl;
                return EXIT_FAILURE;
            }

            // Wait for connected event
            bool initial_connected = selected_device.connected_pp->value();

            if (debug_messages) {
                std::cout << "Init - Connected: " << initial_connected << std::endl;
            }

            {
                std::unique_lock lock(connect_mutex);

                if (initial_connected) {
                    selected_device.connected = true;
                }

                connect_cv.wait_for(lock, 8000ms, [] {
                    return selected_device.connected;
                });
            }

            // Check selected device is connected, exit otherwise
            if (!selected_device.connected) {
                // Remove device
                remove_device_from_cache();

                std::cerr << "Failed to connect to selected device. Exiting.\n";
                return EXIT_FAILURE;
            }


            // Wait for services resolved event
            bool initial_resolved = selected_device.services_resolved_pp->value();
            if (debug_messages) {
                std::cout << "Init - Services resolved: " << initial_resolved << std::endl;
            }

            {
                std::unique_lock lock(connect_mutex);

                if (initial_resolved) {
                    selected_device.services_resolved = true;
                }

                connect_cv.wait_for(lock, 8000ms, [] {
                    return selected_device.services_resolved;
                });
            }

            if (debug_messages) {
                std::cout << "Device bonded: " << std::boolalpha << selected_device.bonded << std::noboolalpha << "\n";
                std::cout << "Device connected: " << std::boolalpha << selected_device.connected << std::noboolalpha <<
                        "\n";
                std::cout << "Services resolved: " << std::boolalpha << selected_device.services_resolved <<
                        std::noboolalpha << "\n";
            }

            if (!selected_device.services_resolved) {
                // Remove device
                remove_device_from_cache();

                std::cerr << "Failed to resolve services. Exiting.\n";
                return EXIT_FAILURE;
            }
        } else {
            if (debug_messages) {
                std::cout << "Device is already connected and services are resolved.\n";
            }
        }


        // Retrieve GATT services from managed objects
        managed_objects_result = bluez_root_op->getObjectManagerInterface()->GetManagedObjects();
        if (debug_messages) {
            std::cout << "Managed objects retrieved.\n";
        }

        // Check for glucose service GATT services
        if (debug_messages) {
            std::cout << "Checking glucose GATT service" << '\n';
        }
        check_gatt_services(selected_device.path, managed_objects_result);


        // Set up glucose measurement characteristic

        // Create proxy object to glucose measurement characteristic
        auto glucose_measurement_characteristic_op = system_bus_connection->create_object_proxy(
            "org.bluez", glucose_measurement_characteristic.path);
        if (debug_messages) {
            std::cout << "glucose measurement characteristic object proxy created.\n";
        }

        // Create signal proxy for glucose measurement characteristic properties changed
        auto glucose_measurement_properties_changed_sp = glucose_measurement_characteristic_op->create_signal<
            PropertiesChangedSignature>(
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged");

        // Register listener for glucose measurement characteristic properties changed signal proxy
        DBus::Path gmc_path = glucose_measurement_characteristic_op->path();
        glucose_measurement_properties_changed_sp->connect(
            sigc::bind(sigc::ptr_fun(&on_properties_changed_for_glucose_measurement), gmc_path));
        if (debug_messages) {
            std::cout << "glucose measurement characteristic properties changed signal proxy registered\n";
        }

        // Call StartNotify
        auto glucose_measurement_start_notify_mp = glucose_measurement_characteristic_op->create_method<void()>(
            "org.bluez.GattCharacteristic1",
            "StartNotify"
        );
        (*glucose_measurement_start_notify_mp)();


        // Set up glucose measurement context characteristic, if supported by device

        std::shared_ptr<DBus::ObjectProxy> glucose_measurement_context_characteristic_op;
        std::shared_ptr<DBus::SignalProxy<PropertiesChangedSignature> >
                glucose_measurement_context_properties_changed_sp;
        std::shared_ptr<DBus::MethodProxy<void()> > glucose_measurement_context_start_notify_mp;

        if (!glucose_measurement_context_characteristic.uuid.empty()) {
            // Create proxy object to glucose measurement context characteristic
            glucose_measurement_context_characteristic_op = system_bus_connection->create_object_proxy(
                "org.bluez", glucose_measurement_context_characteristic.path);
            if (debug_messages) {
                std::cout << "glucose measurement context characteristic object proxy created.\n";
            }
            // Create signal proxy for glucose measurement context characteristic properties changed
            glucose_measurement_context_properties_changed_sp =
                    glucose_measurement_context_characteristic_op->create_signal<PropertiesChangedSignature>(
                        "org.freedesktop.DBus.Properties",
                        "PropertiesChanged");

            // Register listener for glucose measurement context characteristic properties changed signal proxy
            DBus::Path gmcc_path = glucose_measurement_context_characteristic_op->path();
            glucose_measurement_context_properties_changed_sp->connect(
                sigc::bind(sigc::ptr_fun(&on_properties_changed_for_glucose_measurement_context), gmcc_path));
            if (debug_messages) {
                std::cout << "glucose measurement context characteristic properties changed signal proxy registered\n";
            }
            // Call StartNotify
            glucose_measurement_context_start_notify_mp = glucose_measurement_context_characteristic_op->
                    create_method<void()>(
                        "org.bluez.GattCharacteristic1",
                        "StartNotify"
                    );
            (*glucose_measurement_context_start_notify_mp)();
        }

        // Set up record access control point characteristic

        // Create proxy object to  RACP characteristic
        auto racp_characteristic_op = system_bus_connection->create_object_proxy(
            "org.bluez", racp_characteristic.path);
        if (debug_messages) {
            std::cout << "RACP characteristic object proxy created\n";
        }
        // Create signal proxy for RACP characteristic properties changed
        auto racp_properties_changed_sp = racp_characteristic_op->create_signal<PropertiesChangedSignature>(
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged");

        // Register listener for RACP characteristic properties changed signal proxy
        DBus::Path racp_path = racp_characteristic_op->path();
        racp_properties_changed_sp->connect(sigc::bind(sigc::ptr_fun(&on_properties_changed_for_racp), racp_path));
        if (debug_messages) {
            std::cout << "RACP characteristic properties changed signal proxy registered\n";
        }
        // Call StartNotify
        auto racp_start_notify_mp = racp_characteristic_op->create_method<void()>(
            "org.bluez.GattCharacteristic1",
            "StartNotify"
        );
        (*racp_start_notify_mp)();


        // Send command to start receiving all records

        using WriteValueMethod = void(
            std::vector<uint8_t>,
            std::map<std::string, DBus::Variant>);

        auto write_value = racp_characteristic_op->create_method<WriteValueMethod>(
            "org.bluez.GattCharacteristic1",
            "WriteValue"
        );


        std::map<std::string, DBus::Variant> options;
        options["type"] = DBus::Variant(std::string("request"));


        // // 1. Send "Report Number of Stored Records" (Opcode 0x04, Operator 0x01 = All records)
        // const std::vector<uint8_t> count_cmd{0x04, 0x01};
        // auto count_call = write_value->create_call_message();
        // count_call << count_cmd << options;
        // auto return_message =write_value->call(count_call, 5000);

        // Wait briefly for device to complete re-init of connection, if bonding exists before app executes.
        // Certain glucose meter(s) initiate queries against GATT services (GAP, CTS) towards host server
        std::this_thread::sleep_for(500ms);

        const std::vector<uint8_t> &data = {1, 1};

        std::cout << "Starting reading measurements from device\n";

        try {
            (*write_value)(data, options);
            if (debug_messages) {
                std::cout << "Sent request to device.\n";
            }
        } catch (const DBus::Error &e) {
            std::cerr << "WriteValue failed: " << e.name() << " - " << e.message() << std::endl;
        }


        auto then = std::chrono::steady_clock::now();

        std::unique_lock lock(writing_mutex);
        writing_cv.wait_for(lock, 25000ms, [] { return racp_writing_completed; });

        if (racp_writing_completed) {
            if (debug_messages) {
                std::cout << "RACP write completed.\n";
            }
            auto now = std::chrono::steady_clock::now();

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
            std::chrono::duration<double> sec = now - then;
            if (debug_messages) {
                std::cout << "Elapsed (integer ms): " << ms.count() << " ms\n";
                std::cout << "Elapsed (fractional sec): " << sec.count() << " s\n";
            }
        } else {
            std::cerr << "RACP write not completed.\n";
            std::cerr << "Read " << measurements.size() << " measurements" << std::endl;
            std::cerr << "Try to reconnect the device\n";
            remove_device_from_cache();
            return EXIT_FAILURE;
        }

        std::cout << "Read " << measurements.size() << " measurements" << std::endl;

        /*
                // Stop notifications on characteristics
                auto racp_stop_notify_mp = racp_characteristic_op->create_method<void()>(
                    "org.bluez.GattCharacteristic1",
                    "StopNotify"
                );
                try {
                    (*racp_stop_notify_mp)();
                    std::cout << "Sent RACP StopNotify to device.\n";
                } catch (const DBus::Error &e) {
                    std::cerr << "RACP StopNotify failed: " << e.name() << " - " << e.message() << std::endl;
                }


                if (!glucose_measurement_context_characteristic.uuid.empty()) {
                    auto glucose_measurement_context_stop_notify_mp = glucose_measurement_context_characteristic_op->
                            create_method<void()>(
                                "org.bluez.GattCharacteristic1",
                                "StopNotify"
                            );
                    try {
                        (*glucose_measurement_context_stop_notify_mp)();
                        std::cout << "Sent glucose measurement context StopNotify to device.\n";
                    } catch (const DBus::Error &e) {
                        std::cerr << "glucose measurement context StopNotify failed: " << e.name() << " - " << e.message() <<
                                std::endl;
                    }
                }

                auto glucose_measurement_stop_notify_mp = glucose_measurement_characteristic_op->create_method<void()>(
                    "org.bluez.GattCharacteristic1",
                    "StopNotify"
                );
                try {
                    (*glucose_measurement_stop_notify_mp)();
                    std::cout << "Sent glucose measurement StopNotify to device.\n";
                } catch (const DBus::Error &e) {
                    std::cerr << "glucose measurement StopNotify failed: " << e.name() << " - " << e.message() << std::endl;
                }

        */
        // Clean up connection
        auto disconnect_method_proxy = selected_device.op->create_method<void()>("org.bluez.Device1", "Disconnect");
        //auto disconnect_call = disconnect_method_proxy->create_call_message();

        // Disconnect device
        try {
            auto return_future_message = disconnect_method_proxy->call_async();
            system_bus_connection->flush();
            if (debug_messages) {
                std::cout << "Device disconnected" << std::endl;
            }
        } catch ([[maybe_unused]] const DBus::Error &e) {
            std::cerr << "Device disconnection failure." << std::endl;
            return EXIT_FAILURE;
        }

        write_measurements_to_file();
    } catch (const std::exception &e) {
        std::cerr << "D-Bus Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
