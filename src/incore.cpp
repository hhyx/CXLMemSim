//
// Created by victoryang00 on 1/14/23.
//

#include "incore.h"
#include "helper.h"

void pcm_cpuid(const unsigned leaf, CPUID_INFO *info) {
    __asm__ __volatile__("cpuid"
                         : "=a"(info->reg.eax), "=b"(info->reg.ebx), "=c"(info->reg.ecx), "=d"(info->reg.edx)
                         : "a"(leaf));
}

int Incore::start() {
    int i, r = -1;

    for (i = 0; i < 7; i++) {
        r = this->perf[i].start();
        if (r < 0) {
            LOG(ERROR) << fmt::format("perf_start failed. i:{}\n", i);
            return r;
        }
    }
    return r;
}
int Incore::stop() {
    int i, r = -1;

    for (i = 0; i < 7; i++) {
        r = this->perf[i].stop();
        if (r < 0) {
            LOG(ERROR) << fmt::format("perf_stop failed. i:{}\n", i);
            return r;
        }
    }
    return r;
}
void Incore::init_all_dram_rds(const pid_t pid, const int cpu) {
    this->perf[0] = init_incore_perf(pid, cpu, perf_config->all_dram_rds_config, perf_config->all_dram_rds_config1);
}
void Incore::init_cpu_l2stall(const pid_t pid, const int cpu) {
    this->perf[1] = init_incore_perf(pid, cpu, perf_config->cpu_l2stall_config, 0);
}
void Incore::init_cpu_llcl_hits(const pid_t pid, const int cpu) {
    this->perf[2] = init_incore_perf(pid, cpu, perf_config->cpu_llcl_hits_config, 0);
}
void Incore::init_cpu_llcl_miss(const pid_t pid, const int cpu) {
    this->perf[3] = init_incore_perf(pid, cpu, perf_config->cpu_llcl_miss_config, 0);
}
void Incore::init_cpu_mem_read(const pid_t pid, const int cpu) {
    this->perf[4] = init_incore_perf(pid, cpu, perf_config->cpu_bandwidth_read_config, 0);
}
void Incore::init_cpu_mem_write(const pid_t pid, const int cpu) {
    this->perf[5] = init_incore_perf(pid, cpu, perf_config->cpu_bandwidth_write_config, 0);
}
void Incore::init_cpu_mmap_count(const pid_t pid, const int cpu) { this->perf[6] = init_incore_bpf_perf(pid, cpu); }
int Incore::read_cpu_elems(struct CPUElem *elem) {
    ssize_t r;

    r = this->perf[0].read_pmu(&elem->all_dram_rds);
    if (r < 0) {
        LOG(ERROR) << fmt::format("%s read all_dram_rds failed.\n", __func__);
        return r;
    }
    LOG(DEBUG) << fmt::format("read all_dram_rds:{}\n", elem->all_dram_rds);

    r = this->perf[1].read_pmu(&elem->cpu_l2stall_t);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_l2stall_t failled.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_l2stall_t:{}\n", elem->cpu_l2stall_t);

    r = this->perf[2].read_pmu(&elem->cpu_llcl_hits);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_llcl_hits failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_llcl_hits:{}\n", elem->cpu_llcl_hits);

    r = this->perf[3].read_pmu(&elem->cpu_llcl_miss);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_llcl_miss failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_llcl_miss:{}\n", elem->cpu_llcl_miss);

    r = this->perf[4].read_pmu(&elem->cpu_bandwidth_read);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_bandwidth_read failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_bandwidth_read:{}\n", elem->cpu_bandwidth_read);
    r = this->perf[5].read_pmu(&elem->cpu_bandwidth_write);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_bandwidth_write failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_bandwidth_write:{}\n", elem->cpu_bandwidth_write);
    elem->cpu_mmap_address_length = this->perf[6].read_trace_pipe();
}
Incore::Incore(const pid_t pid, const int cpu, struct PerfConfig *perf_config) : perf_config(perf_config) {
    /* reset all pmc values */
    this->init_all_dram_rds(pid, cpu);
    this->init_cpu_l2stall(pid, cpu);
    this->init_cpu_llcl_hits(pid, cpu);
    this->init_cpu_llcl_miss(pid, cpu);
    this->init_cpu_mem_read(pid, cpu);
    this->init_cpu_mem_write(pid, cpu);
    this->init_cpu_mmap_count(pid, cpu);
}