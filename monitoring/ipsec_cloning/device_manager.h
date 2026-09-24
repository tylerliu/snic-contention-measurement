#ifndef MONITORING_APP_DEVICE_MANAGER_H
#define MONITORING_APP_DEVICE_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct doca_dev;
struct doca_dev_rep;

class DeviceManager {
public:
	DeviceManager();
	~DeviceManager();

	DeviceManager(const DeviceManager &) = delete;
	DeviceManager &operator=(const DeviceManager &) = delete;
	DeviceManager(DeviceManager &&) = delete;
	DeviceManager &operator=(DeviceManager &&) = delete;

	uint16_t add_device(const std::string &device_str, const char *label, bool has_dpdk_port = false);
	uint16_t add_device_rep(const std::string &device_str, const char *label, bool rep_has_dpdk_port = false);

	doca_dev *get_device(uint16_t port_id) const;
	doca_dev_rep *get_device_rep(uint16_t port_id) const;
	uint16_t get_parent_port_id(uint16_t port_id) const;
	bool is_device_rep(uint16_t port_id) const;

	bool has_dpdk_port(uint16_t port_id) const;
	inline uint16_t get_port_count() const { return ports_.size(); }
	inline uint16_t get_dpdk_port_count() const { return dpdk_device_count_; }

private:
	struct PortRecord {
		enum class Kind { Device, Rep };
		Kind kind{Kind::Device};
		doca_dev *device{nullptr};
		doca_dev_rep *rep{nullptr};
		uint16_t parent_port_id{0};
		bool has_dpdk_port{false};
	};

	bool is_non_dpdk_device_exist_already() const;

	/**
	 * Find an existing device
	 *
	 * @predicate [in]: predicate function
	 * @return: port ID of the device, UINT16_MAX if not found
	 */
	uint16_t find_existing_device(std::function<bool(doca_dev *)> predicate);

	/**
	 * Find an existing representor
	 *
	 * @predicate [in]: predicate function
	 * @return: port ID of the representor, UINT16_MAX if not found
	 */
	uint16_t find_existing_device_rep(std::function<bool(doca_dev_rep *)> predicate);

	/**
	 * Scan existing devices for a representor
	 *
	 * @dev_name [in]: device name string
	 * @out_rep [out]: pointer to the representor, NULL if not found
	 * @return: port ID of the parent device, UINT16_MAX if not found
	 */
	uint16_t scan_existing_devices_for_rep(const std::string &dev_name, doca_dev_rep **out_rep);

	void probe_dpdk_device(doca_dev *dev, const std::string &dev_args);
	void probe_dpdk_device_rep(doca_dev *dev_parent, doca_dev_rep *rep, const std::string &device_str);

	std::vector<PortRecord> ports_;
	uint16_t dpdk_device_count_;
};

#endif /* MONITORING_APP_DEVICE_MANAGER_H */

