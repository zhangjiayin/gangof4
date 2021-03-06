﻿#include "quote_cmn_utility.h"

using namespace std;

IPAndPortNum ParseIPAndPortNum(const std::string &addr_cfg)
{
    //format: udp://192.168.60.23:7120   or  tcp://192.168.60.23:7120
    std::string ip_port = addr_cfg.substr(6);
    std::size_t split_pos = ip_port.find(":");
    if ((split_pos == std::string::npos) || (split_pos + 1 >= ip_port.length()))
    {
        MY_LOG_ERROR("parse address failed: %s", addr_cfg.c_str());
        return std::make_pair("", (unsigned short) 0);
    }

    std::string addr_ip = ip_port.substr(0, split_pos);
    std::string addr_port = ip_port.substr(split_pos + 1);
    int port_tmp = atoi(addr_port.c_str());
    if (port_tmp <= 0 || port_tmp > 0xFFFF)
    {
        MY_LOG_ERROR("port in address beyond valid range: %s", addr_cfg.c_str());
        return std::make_pair("", 0);
    }

    return std::make_pair(addr_ip, (unsigned short) port_tmp);
}

IPAndPortStr ParseIPAndPortStr(const std::string &addr_cfg)
{
    //format: udp://192.168.60.23:7120   or  tcp://192.168.60.23:7120
    std::string ip_port = addr_cfg.substr(6);
    std::size_t split_pos = ip_port.find(":");
    if ((split_pos == std::string::npos) || (split_pos + 1 >= ip_port.length()))
    {
        MY_LOG_ERROR("parse address failed: %s", addr_cfg.c_str());
        return std::make_pair("", "");
    }

    std::string addr_ip = ip_port.substr(0, split_pos);
    std::string addr_port = ip_port.substr(split_pos + 1);
    int port_tmp = atoi(addr_port.c_str());
    if (port_tmp <= 0 || port_tmp > 0xFFFF)
    {
        MY_LOG_ERROR("port in address beyond valid range: %s", addr_cfg.c_str());
    }

    return std::make_pair(addr_ip, addr_port);
}

void QuoteUpdateState(const char *name, int s)
{
    update_state(name, TYPE_QUOTE, s, GetDescriptionWithState(s).c_str());
    MY_LOG_INFO("update_state: name: %s, State: %d, Description: %s.", name, s, GetDescriptionWithState(s).c_str());
}
