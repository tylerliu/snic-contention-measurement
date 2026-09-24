#include "device_manager.h"
#include "doca_types.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_error.h>
#include <doca_log.h>

#include <rte_eal.h>
#include <rte_ethdev.h>

extern "C" {
#include "utils.h"
}

DOCA_LOG_REGISTER(DEVICE_MANAGER);

DeviceManager::DeviceManager()
	: dpdk_device_count_(0)
{
}

DeviceManager::~DeviceManager()
{
	for (auto it = ports_.rbegin(); it != ports_.rend(); ++it) {
		PortRecord &record = *it;
		if (record.kind == PortRecord::Kind::Rep && record.rep != nullptr) {
			doca_dev_rep_close(record.rep);
			record.rep = nullptr;
		} else if (record.kind == PortRecord::Kind::Device && record.device != nullptr) {
			doca_dev_close(record.device);
			record.device = nullptr;
		}
	}
}

uint16_t DeviceManager::add_device(const std::string &device_str, const char *label, bool has_dpdk_port)
{
	if (has_dpdk_port && is_non_dpdk_device_exist_already()) {
		throw std::runtime_error("Non-DPDK device can only be added after DPDK device");
	}

	size_t pos = device_str.find(',');
	std::string dev_name = device_str.substr(0, pos);
	std::string dev_args = pos != std::string::npos ? device_str.substr(pos + 1) : "";
	doca_dev *dev = nullptr;

	if (dev_name.find("auxiliary:mlx5_core.sf.") == 0) {
		size_t pos = dev_name.find_last_of('.');
		uint32_t sf_index = std::stoi(dev_name.substr(pos + 1));
		uint16_t existing_device_port_id = find_existing_device([sf_index](doca_dev *dev) {
			struct doca_devinfo *devinfo = doca_dev_as_devinfo(dev);
			enum doca_pci_func_type pci_func_type;
			doca_devinfo_get_pci_func_type(devinfo, &pci_func_type);
			if (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
				uint32_t sf_index_found;
				doca_devinfo_get_sf_index(devinfo, &sf_index_found);
				return sf_index_found == sf_index;
			}
			return false;
		});
		if (existing_device_port_id != UINT16_MAX) {
			if (ports_[existing_device_port_id].has_dpdk_port != has_dpdk_port) {
				DOCA_LOG_ERR("%s device with SF %u is already configured with DPDK", label, sf_index);
				throw std::runtime_error(std::string(label) + " device with SF " + std::to_string(sf_index) + " is already configured with DPDK");
			}
			return existing_device_port_id;
		}

		DOCA_LOG_INFO("Probing %s DOCA device with SF index: %u", label, sf_index);
		doca_error_t result = open_doca_device_with_sf_index(sf_index, NULL, &dev);
		if (result != DOCA_SUCCESS) {
			throw std::runtime_error("Failed to probe " + std::string(label) + " DOCA device with SF index: " + dev_name);
		}
	} else if (dev_name.find(":") != std::string::npos) {
		uint16_t existing_device_port_id = find_existing_device([dev_name](doca_dev *dev) {
			struct doca_devinfo *devinfo = doca_dev_as_devinfo(dev);
			enum doca_pci_func_type pci_func_type;
			doca_devinfo_get_pci_func_type(devinfo, &pci_func_type);
			if (pci_func_type == DOCA_PCI_FUNC_TYPE_PF) {
				uint8_t is_equal = 0;
				doca_devinfo_is_equal_pci_addr(devinfo, (const char *)dev_name.c_str(), &is_equal);
				return is_equal == 1;
			}
			return false;
		});
		if (existing_device_port_id != UINT16_MAX) {
			if (ports_[existing_device_port_id].has_dpdk_port != has_dpdk_port) {
				DOCA_LOG_ERR("%s device with PCI address %s is already configured with DPDK", label, dev_name.c_str());
				throw std::runtime_error(std::string(label) + " device with PCI address " + dev_name + " is already configured with DPDK");
			}
			return existing_device_port_id;
		}

		DOCA_LOG_INFO("Probing %s DOCA device with PCI address: %s", label, dev_name.c_str());
		doca_error_t result = open_doca_device_with_pci(dev_name.c_str(), NULL, &dev);
		if (result != DOCA_SUCCESS) {
			throw std::runtime_error("Failed to probe " + std::string(label) + " DOCA device with PCI address: " + dev_name);
		}
	} else {
		uint16_t existing_device_port_id = find_existing_device([dev_name](doca_dev *dev) {
			struct doca_devinfo *devinfo = doca_dev_as_devinfo(dev);
			char iface_name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
			doca_devinfo_get_iface_name(devinfo, iface_name, sizeof(iface_name));
			return strncmp(iface_name, (const char *)dev_name.c_str(), dev_name.size()) == 0;
			return false;
		});
		if (existing_device_port_id != UINT16_MAX) {
			if (ports_[existing_device_port_id].has_dpdk_port != has_dpdk_port) {
				DOCA_LOG_ERR("%s device with interface name %s is already configured with DPDK", label, dev_name.c_str());
				throw std::runtime_error(std::string(label) + " device with interface name " + dev_name + " is already configured with DPDK");
			}
			return existing_device_port_id;
		}

		DOCA_LOG_INFO("Probing %s DOCA device with interface name: %s", label, dev_name.c_str());
		doca_error_t result = open_doca_device_with_iface_name((const uint8_t *)dev_name.c_str(), dev_name.size(), NULL, &dev);
		if (result != DOCA_SUCCESS) {
			throw std::runtime_error("Failed to probe " + std::string(label) + " DOCA device with iface: " + dev_name);
		}
	}

	if (has_dpdk_port) {
		probe_dpdk_device(dev, dev_args);
	}

	PortRecord *port_record = new PortRecord();
	port_record->device = dev;
	port_record->has_dpdk_port = has_dpdk_port;
	ports_.push_back(*port_record);
	if (has_dpdk_port && ports_.size() != dpdk_device_count_) {
		throw std::runtime_error("Number of ports does not match the number of DPDK devices");
	}
	return ports_.size() - 1;
}

uint16_t DeviceManager::add_device_rep(const std::string &device_str, const char *label, bool has_dpdk_port)
{
	if (has_dpdk_port && is_non_dpdk_device_exist_already()) {
		throw std::runtime_error("Non-DPDK device can only be added after DPDK device");
	}

	size_t pos = device_str.find(',');
	std::string dev_name = device_str.substr(0, pos);
	std::string dev_args = pos != std::string::npos ? device_str.substr(pos + 1) : "";
	doca_dev *dev = nullptr;
	doca_dev_rep *rep = nullptr;

	uint16_t existing_device_port_id = find_existing_device_rep([dev_name](doca_dev_rep *rep) {
		if (dev_name.find("auxiliary:mlx5_core.sf.") == 0) {
			size_t pos = dev_name.find_last_of('.');
			uint32_t sf_index = std::stoi(dev_name.substr(pos + 1));
			enum doca_pci_func_type pci_func_type;
			doca_devinfo_rep *devinfo_rep = doca_dev_rep_as_devinfo(rep);
			doca_devinfo_rep_get_pci_func_type(devinfo_rep, &pci_func_type);
			if (pci_func_type != DOCA_PCI_FUNC_TYPE_SF) {
				return false;
			}
			uint32_t sf_index_found;
			doca_devinfo_rep_get_sf_index(devinfo_rep, &sf_index_found);
			return sf_index_found == sf_index;
		} else {
			char iface_name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
			doca_devinfo_rep *devinfo_rep = doca_dev_rep_as_devinfo(rep);
			doca_devinfo_rep_get_iface_name(devinfo_rep, iface_name, sizeof(iface_name));
			return strncmp(iface_name, (const char *)dev_name.c_str(), dev_name.size()) == 0;
		}
	});
	if (existing_device_port_id != UINT16_MAX) {
		if (ports_[existing_device_port_id].has_dpdk_port != has_dpdk_port) {
			DOCA_LOG_ERR("%s representor with interface name %s is already configured with DPDK", label, dev_name.c_str());
			throw std::runtime_error(std::string(label) + " representor with interface name " + dev_name + " is already configured with DPDK");
		}
		return existing_device_port_id;
	}

	uint16_t parent_port_id = scan_existing_devices_for_rep(dev_name, &rep);
	if (rep == nullptr) {
		if (dev_name.find("auxiliary:mlx5_core.sf.") == 0) {
			size_t pos = dev_name.find_last_of('.');
			uint32_t sf_index = std::stoi(dev_name.substr(pos + 1));

			DOCA_LOG_INFO("Probing %s DOCA representor with SF index: %u", label, sf_index);
			doca_error_t result = open_doca_rep_with_sf_index(sf_index, NULL, &dev, &rep);
			if (result != DOCA_SUCCESS) {
				throw std::runtime_error("Failed to probe " + std::string(label) + " DOCA representor with SF index: " + dev_name);
			}
		} else {
			DOCA_LOG_INFO("Probing %s DOCA representor with interface name: %s", label, dev_name.c_str());
			doca_error_t result = open_doca_device_rep_with_iface_name((const uint8_t *)dev_name.c_str(), dev_name.size(), NULL, &dev, &rep);
			if (result != DOCA_SUCCESS) {
				throw std::runtime_error("Failed to probe " + std::string(label) + " DOCA representor with interface name: " + dev_name);
			}
		}
	}

	if (has_dpdk_port) {
		auto parent_dev = dev;
		if (parent_dev == nullptr) {
			parent_dev = ports_[parent_port_id].device;
		}
		probe_dpdk_device_rep(parent_dev, rep, dev_args);
	}

	if (dev != nullptr) {
		// create port record for the parent device
		PortRecord *parent_port_record = new PortRecord();
		parent_port_record->device = dev;
		parent_port_record->has_dpdk_port = true;
		ports_.push_back(*parent_port_record);
		parent_port_id = ports_.size() - 1;
	}

	PortRecord *port_record = new PortRecord();
	port_record->rep = rep;
	port_record->kind = PortRecord::Kind::Rep;
	port_record->has_dpdk_port = has_dpdk_port;
	port_record->parent_port_id = parent_port_id;
	ports_.push_back(*port_record);
	if (has_dpdk_port && ports_.size() != dpdk_device_count_) {
		throw std::runtime_error("Number of ports does not match the number of DPDK devices");
	}
	return ports_.size() - 1;
}

doca_dev *DeviceManager::get_device(uint16_t port_id) const
{
	assert(ports_[port_id].kind == PortRecord::Kind::Device);
	return ports_[port_id].device;
}
doca_dev_rep *DeviceManager::get_device_rep(uint16_t port_id) const
{
	assert(ports_[port_id].kind == PortRecord::Kind::Rep);
	return ports_[port_id].rep;
}

uint16_t DeviceManager::get_parent_port_id(uint16_t port_id) const
{
	assert(ports_[port_id].kind == PortRecord::Kind::Rep);
	return ports_[port_id].parent_port_id;
}

bool DeviceManager::is_device_rep(uint16_t port_id) const
{
	return ports_[port_id].kind == PortRecord::Kind::Rep;
}

bool DeviceManager::has_dpdk_port(uint16_t port_id) const
{
	return ports_[port_id].has_dpdk_port;
}

bool DeviceManager::is_non_dpdk_device_exist_already() const
{
	return dpdk_device_count_ != ports_.size();
}

uint16_t DeviceManager::find_existing_device(std::function<bool(doca_dev *)> predicate)
{
	for (uint16_t i = 0; i < ports_.size(); ++i) {
		if (ports_[i].kind == PortRecord::Kind::Device && predicate(ports_[i].device)) {
			return i;
		}
	}
	return UINT16_MAX;
}

uint16_t DeviceManager::find_existing_device_rep(std::function<bool(doca_dev_rep *)> predicate)
{
	for (uint16_t i = 0; i < ports_.size(); ++i) {
		if (ports_[i].kind == PortRecord::Kind::Rep && predicate(ports_[i].rep)) {
			return i;
		}
	}
	return UINT16_MAX;
}

uint16_t DeviceManager::scan_existing_devices_for_rep(const std::string &dev_name, doca_dev_rep **out_rep)
{
	for (uint16_t i = 0; i < ports_.size(); ++i) {
		if (ports_[i].kind == PortRecord::Kind::Device) {
			doca_dev *dev = ports_[i].device;
			struct doca_devinfo_rep **rep_list = NULL;
			uint32_t nb_rdevs = 0;
			doca_error_t result = doca_devinfo_rep_create_list(dev, DOCA_DEVINFO_REP_FILTER_NET, &rep_list, &nb_rdevs);
			if (result != DOCA_SUCCESS) {
				throw std::runtime_error("Failed to create devinfo representor list for " + dev_name);
			}
			for (uint32_t j = 0; j < nb_rdevs; ++j) {
				doca_devinfo_rep *devinfo_rep = rep_list[j];
				if (dev_name.find("auxiliary:mlx5_core.sf.") == 0) {
					size_t pos = dev_name.find_last_of('.');
					uint32_t sf_index = std::stoi(dev_name.substr(pos + 1));
					enum doca_pci_func_type pci_func_type;
					doca_devinfo_rep_get_pci_func_type(devinfo_rep, &pci_func_type);
					if (pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
						uint32_t sf_index_found;
						doca_devinfo_rep_get_sf_index(devinfo_rep, &sf_index_found);
						if (sf_index_found == sf_index) {
							doca_dev_rep_open(devinfo_rep, out_rep);
							if (result != DOCA_SUCCESS) {
								throw std::runtime_error("Failed to open representor for " + dev_name);
							}
							return i;
						}
					} else {
						continue;
					}
				} else {
					char iface_name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
					doca_devinfo_rep_get_iface_name(devinfo_rep, iface_name, sizeof(iface_name));
					if (strncmp(iface_name, (const char *)dev_name.c_str(), dev_name.size()) == 0) {
						doca_dev_rep_open(devinfo_rep, out_rep);
						if (result != DOCA_SUCCESS) {
							throw std::runtime_error("Failed to open representor for " + dev_name);
						}
						return i;
					} else {
						continue;
					}
				}
			}
			doca_devinfo_rep_destroy_list(rep_list);
		}
	}
	out_rep = nullptr;
	return UINT16_MAX;
}

void DeviceManager::probe_dpdk_device(doca_dev *dev, const std::string &dev_args)
{
	uint16_t nb_ports = rte_eth_dev_count_avail();
	doca_dpdk_port_probe(dev, dev_args.c_str());
	uint16_t new_nb_ports = rte_eth_dev_count_avail();
	if (new_nb_ports == nb_ports) {
		throw std::runtime_error("Failed to probe DPDK device");
	}
	dpdk_device_count_ = new_nb_ports;
}

void DeviceManager::probe_dpdk_device_rep(doca_dev *dev_parent, doca_dev_rep *rep, const std::string &dev_args)
{
	uint16_t nb_ports = rte_eth_dev_count_avail();
	doca_dpdk_port_probe_with_representors(dev_parent, dev_args.c_str(), &rep, 1);
	uint16_t new_nb_ports = rte_eth_dev_count_avail();
	if (new_nb_ports == nb_ports) {
		throw std::runtime_error("Failed to probe DPDK device");
	} else if (new_nb_ports > nb_ports + 2) {
		throw std::runtime_error("Number of ports does not match the number of DPDK devices");
	}
	dpdk_device_count_ = new_nb_ports;
}