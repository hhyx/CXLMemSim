//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLCONTROLLER_H
#define CXL_MEM_SIMULATOR_CXLCONTROLLER_H

#include "cxlcounter.h"
#include "cxlendpoint.h"
#include "policy.h"
#include <cstdint>
#include <string_view>
#include <vector>

class CXLController : CXLSwitch {
public:
    std::vector<CXLMemExpander *> expanders{};
    uint64_t capacity;
    Policy policy;
    int num_switches;
    CXLController(Policy policy);
    void construct_topo(std::string_view newick_tree);
    void insert_end_point(CXLMemExpander *end_point);
    std::vector<std::string> tokenize(const std::string_view &s);
    double calculate_latency(double weight, struct Elem *elem); // traverse the tree to calculate the latency
    double calculate_bandwidth(double weight, struct Elem *elem);
    void insert(uint64_t timestamp, uint64_t size);
    void delete_entry(uint64_t addr);
    void output() override;
};

#endif // CXL_MEM_SIMULATOR_CXLCONTROLLER_H
