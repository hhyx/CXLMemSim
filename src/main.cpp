//
// Created by victoryang00 on 1/12/23.
//
#include "cxlendpoint.h"
#include "helper.h"
#include "logging.h"
#include "monitor.h"
#include "policy.h"
#include <cerrno>
#include <cinttypes>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cxxopts.hpp>
#include <fcntl.h>
#include <getopt.h>
#include <range/v3/view.hpp>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/cxl_mem_simulator.sock"

int main(int argc, char *argv[]) {

    cxxopts::Options options("CXL-MEM-Simulator",
                             "For simulation of CXL.mem Type 3 on Broadwell, Skylake, and Saphire Rapids");
    options.add_options()("t,target", "The script file to execute", cxxopts::value<std::string>()->default_value("ls"))(
        "h,help", "The value for epoch value", cxxopts::value<bool>()->default_value("false"))(
        "i,interval", "The value for epoch value", cxxopts::value<int>()->default_value("20"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on", cxxopts::value<std::vector<int>>()->default_value("0,1"))(
        "d,dram_latency", "The current platform's dram latency", cxxopts::value<double>()->default_value("85"))(
        "p,pebsperiod", "The pebs sample period", cxxopts::value<int>()->default_value("1"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("p"))(
        "to,topology", "The newick tree input for the CXL memory expander topology",
        cxxopts::value<std::string>()->default_value("(1)"))("f,frequency", "The frequency for the running thread",
                                                             cxxopts::value<double>()->default_value("4000"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100,150"))(
        "w,weight", "The simulated weight for multiplying with the LLC miss",
        cxxopts::value<double>()->default_value("4.1"))("b,bandwidth", "The simulated bandwidth by linear regression",
                                                        cxxopts::value<std::vector<int>>()->default_value("50,50"));

    auto result = options.parse(argc, argv);
    if (result["help"].as<bool>()) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
    auto interval = result["interval"].as<int>();
    auto cpuset = result["cpuset"].as<std::vector<int>>();
    auto pebsperiod = result["pebsperiod"].as<int>();
    auto latency = result["latency"].as<std::vector<int>>();
    auto weight = result["weight"].as<double>();
    auto bandwidth = result["bandwidth"].as<std::vector<int>>();
    auto frequency = result["frequency"].as<double>();
    auto topology = result["topology"].as<std::string>();
    auto capacity = result["capacity"].as<std::vector<int>>();
    auto dram_latency = result["dram_latency"].as<double>();
    Helper helper{};
    InterleavePolicy policy{};
    CXLController controller{policy};
    uint64_t use_cpus = 0;
    cpu_set_t use_cpuset;
    CPU_ZERO(&use_cpuset);
    for (auto c : cpuset) {
        use_cpus += std::pow(2, c);
    }
    for (int i = 0; i < helper.cpu; i++) {
        if (!use_cpus || use_cpus & 1UL << i) {
            CPU_SET(i, &use_cpuset);
            LOG(DEBUG) << fmt::format("use cpuid: {}\n", i);
        }
    }
    auto tnum = CPU_COUNT(&use_cpuset);
    auto cur_processes = 0;
    auto ncpu = helper.cpu;
    auto ncbo = helper.cbo;
    auto nmem = capacity.size();
    LOG(DEBUG) << fmt::format("tnum:{}, intrval:{}, weight:{}\n", tnum, interval, weight);
    for (auto const &[idx, value] : weight | ranges::views::enumerate) {
        LOG(DEBUG) << fmt::format("memory_region:{}\n", idx + 1);
        LOG(DEBUG) << fmt::format(" capacity:{}\n", capacity[idx + 1]);
        LOG(DEBUG) << fmt::format(" read_latency:{}\n", latency[idx * 2]);
        LOG(DEBUG) << fmt::format(" write_latency:{}\n", latency[idx * 2 + 1]);
        LOG(DEBUG) << fmt::format(" read_bandwidth:{}\n", bandwidth[idx * 2]);
        LOG(DEBUG) << fmt::format(" write_bandwidth:{}\n", bandwidth[idx * 2 + 1]);
        auto *ep = new CXLMemExpander(idx, latency[idx * 2], latency[idx * 2 + 1], bandwidth[idx * 2],
                                      bandwidth[idx * 2 + 1], idx);
        controller.insert_end_point(ep);
    }
    controller.construct_topo(topology);
    int sock;
    struct sockaddr_un addr {};

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    remove(addr.sun_path);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG(ERROR) << "Failed to execute. Can't bind to a socket.";
        exit(1);
    }
    LOG(DEBUG) << fmt::format("cpu_freq:{}\n", frequency);
    LOG(DEBUG) << fmt::format("num_of_cbo:{}\n", ncbo);
    LOG(DEBUG) << fmt::format("num_of_cpu:{}\n", ncpu);
    Monitors monitors{tnum, &use_cpuset, static_cast<int>(capacity.size()), helper};

    /* zombie avoid */
    Helper::detach_children();
    /* create target process */
    auto t_process = fork();
    if (t_process < 0) {
        LOG(ERROR) << "Fork: failed to create target process";
        exit(1);
    } else if (t_process == 0) {
        // https://stackoverflow.com/questions/24796266/tokenizing-a-string-to-pass-as-char-into-execve
        char cmd_buf[1024] = {0};
        strncpy(cmd_buf, target.c_str(), sizeof(cmd_buf));

        /* This strtok_r() call puts '\0' after the first token in the buffer,
         * It saves the state to the strtok_state and subsequent calls resume from that point. */
        char *strtok_state = nullptr;
        char *filename = strtok_r(cmd_buf, " ", &strtok_state);

        /* Allocate an array of pointers.
         * We will make them point to certain locations inside the cmd_buf. */
        char *args[32] = {nullptr};
        /* loop the strtok_r() call while there are tokens and free space in the array */
        size_t current_arg_idx;
        for (current_arg_idx = 0; current_arg_idx < 32; ++current_arg_idx) {
            /* Note that the first argument to strtok_r() is nullptr.
             * That means resume from a point saved in the strtok_state. */
            char *current_arg = strtok_r(nullptr, " ", &strtok_state);
            if (current_arg == nullptr) {
                break;
            }

            args[current_arg_idx] = current_arg;
            LOG(DEBUG) << fmt::format("args[%d] = %s\n", current_arg_idx, args[current_arg_idx]);
        }
        execv(filename, args);
        /* We do not need to check the return value */
        LOG(ERROR) << "Exec: failed to create target process";
        exit(1);
    }

    // In case of process, use SIGSTOP.
    auto res = monitors.enable(t_process, t_process, true, 0, tnum);
    if (res == -1) {
        LOG(ERROR) << fmt::format("Failed to enable monitor\n");
        exit(0);
    } else if (res < 0) {
        // pid not found. might be already terminated.
        LOG(DEBUG) << fmt::format("pid(%ul) not found. might be already terminated.", t_process);
    }
    cur_processes++;
    LOG(DEBUG) << fmt::format("pid of mes = %d, cur process=%d\n", t_process, cur_processes);

    if (cur_processes >= ncpu) {
        LOG(ERROR) << fmt::format(
            "Failed to execute. The number of processes/threads of the target application is more than "
            "physical CPU cores.\n");
        exit(0);
    }

    // Wait all the target processes until emulation process initialized.
    monitors.stop_all(cur_processes);

    /* get CPU information */
    if (!get_cpu_info(&monitors.mon[0].before->cpuinfo)) {
        LOG(DEBUG) << "Failed to obtain CPU information.\n";
    }

    /* check the CPU model */
    auto perf_config = helper.detect_model(monitors.mon[0].before->cpuinfo.cpu_model);

    PMUInfo pmu{t_process, &helper, &perf_config};

    /* Caculate epoch time */
    struct timespec waittime {};
    waittime.tv_sec = interval / 1000;
    waittime.tv_nsec = (interval % 1000) * 1000000;

    LOG(DEBUG) << "The target process starts running.\n";
    LOG(DEBUG) << fmt::format("set nano sec = %lu\n", waittime.tv_nsec);

    /* read CBo params */
    for (auto mon : monitors.mon) {
        for (auto const &[idx, value] : mon.before->cbos | ranges::views::enumerate) {
            pmu.cbos[idx].read_cbo_elems(&value);
        }
        for (auto const &[idx, value] : mon.before->cpus | ranges::views::enumerate) {
            pmu.cpus[idx].read_cpu_elems(&value);
        }
    }

    uint32_t diff_nsec = 0;
    struct timespec start_ts, end_ts;
    struct timespec sleep_start_ts, sleep_end_ts;

#ifdef VERBOSE_DEBUG
    struct timespec recv_ts;
#endif
    // Wait all the target processes until emulation process initialized.
    monitors.run_all(cur_processes);
    for (int i = 0; i < cur_processes; i++) {
        clock_gettime(CLOCK_MONOTONIC, &monitors.mon[i].start_exec_ts);
    }

    /*
     * The format for receiving an tgid,tid,opcode via a socket is as follows.
     *   |  tgid:32bit  |  tid:32bit  |  opcode:32bit  |  num_of_region:32bit  |
     * To emulate the hybrid memory, specify 2 or more for num_of_region.
     * When specifying 1 or more in num_of_region, add the following format to
     * as repeatedly as the num_of_region in addition to the above.
     *   |  address:64bit  |  size:64bit  |
     */
    enum opcode {
        CXL_MEM_PROCESS_CREATE = 0,
        CXL_MEM_THREAD_CREATE = 1,
        CXL_MEM_THREAD_EXIT = 2,
    };
    struct op_data {
        uint32_t tgid;
        uint32_t tid;
        uint32_t opcode;
        uint32_t num_of_region;
    };
    size_t regs_size = sizeof(CXLMemExpander) * nmem;
    size_t sock_data_size = sizeof(struct op_data) + regs_size;
    size_t sock_buf_size = sock_data_size + 1 /*for size check*/;
    char *sock_buf = (char *)malloc(sock_buf_size);

    while (true) {
        /* wait for pre-defined interval */
        clock_gettime(CLOCK_MONOTONIC, &sleep_start_ts);

#ifdef VERBOSE_DEBUG
        DEBUG_PRINT("sleep_start_ts: %010lu.%09lu\n", sleep_start_ts.tv_sec, sleep_start_ts.tv_nsec);
#endif
        int n;
        auto mon = monitors.mon[0];
        do {
            memset(sock_buf, 0, sock_buf_size);
            // without blocking
            n = recv(sock, sock_buf, sock_buf_size, MSG_DONTWAIT);
            if (n < 1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // no data
                    break;
                } else {
                    LOG(ERROR) << "Failed to recv";
                    exit(0);
                }
            } else if (n >= sizeof(struct op_data) && n <= sock_data_size) {
                struct op_data *opd = (struct op_data *)sock_buf;
                LOG(DEBUG) << fmt::format("received data: size=%d, tgid=%u, tid=%u, opcode=%u, num_of_region=%u\n", n,
                                          opd->tgid, opd->tid, opd->opcode, opd->num_of_region);

                if (opd->opcode == CXL_MEM_THREAD_CREATE || opd->opcode == CXL_MEM_PROCESS_CREATE) {
                    int target;
                    bool is_process = (opd->opcode == CXL_MEM_PROCESS_CREATE) ? true : false;
                    uint64_t period = (opd->num_of_region >= 2) ? pebsperiod : 0; // is hybrid
                    // register to monitor
                    target = monitors.enable(opd->tgid, opd->tid, is_process, period, tnum);
                    if (target == -1) {
                        LOG(ERROR) << "Failed to enable monitor\n";
                        exit(0);
                    } else if (target < 0) {
                        // tid not found. might be already terminated.
                        continue;
                    }
                    mon = monitors.mon[target];
                    if (opd->num_of_region >= 2) { // Ignored if num_of_region is 1 or less
                        // pebs sampling
                        if ((n - sizeof(struct op_data)) != (sizeof(CXLMemExpander) * opd->num_of_region)) {
                            LOG(ERROR) << "Received data is invalid.\n";
                            exit(0);
                        }
                        auto *ri = (CXLMemExpander *)(sock_buf + sizeof(struct op_data));
                        if (mon.set_region_info(opd->num_of_region, ri) < 0) {
                            LOG(ERROR) << "Received data is invalid.\n";
                            exit(0);
                        }
                    }
                    // Wait the target processes until emulation process initialized.
                    mon.stop();
                    /* read CBo params */
                    for (auto j = 0; j < ncbo; j++) {
                        pmu.cbos[j].read_cbo_elems(&mon.before->cbos[j]);
                    }
                    for (auto j = 0; j < ncpu; j++) {
                        pmu.cpus[j].read_cpu_elems(&mon.before->cpus[j]);
                    }
                    // Run the target processes.
                    mon.run();
                    clock_gettime(CLOCK_MONOTONIC, &mon.start_exec_ts);
                } else if (opd->opcode == CXL_MEM_THREAD_EXIT) {
                    // unregister from monitor, and display results.
                    if (monitors.terminate(opd->tgid, opd->tid, tnum) < 0) {
                        LOG(ERROR) << "It might be already terminated.\n";
                    }
                }
            } else {
                LOG(ERROR) << fmt::format("received data is invalid size: size=%d\n", n);
                exit(0);
            }
        } while (n > 0); // check the next message.

#ifdef VERBOSE_DEBUG
        clock_gettime(CLOCK_MONOTONIC, &recv_ts);
        LOG(DEBUG) << fmt::format("recv_ts       : %010lu.%09lu\n", recv_ts.tv_sec, recv_ts.tv_nsec);
        if (recv_ts.tv_nsec < sleep_start_ts.tv_nsec) {
            LOG(DEBUG) << fmt::format("start - recv  : %10lu.%09lu\n", recv_ts.tv_sec - sleep_start_ts.tv_sec - 1,
                                      recv_ts.tv_nsec + 1000000000 - sleep_start_ts.tv_nsec);
        } else {
            LOG(DEBUG) << fmt::format("start - recv  : %10lu.%09lu\n", recv_ts.tv_sec - sleep_start_ts.tv_sec,
                                      recv_ts.tv_nsec - sleep_start_ts.tv_nsec);
        }
#endif

        struct timespec req = waittime;
        struct timespec rem = {0};
        while (true) {
            auto ret = nanosleep(&req, &rem);
            if (ret == 0) { // success
                break;
            } else { // ret < 0
                if (errno == EINTR) {
                    // The pause has been interrupted by a signal that was delivered to the thread.
                    LOG(ERROR) << fmt::format("nanosleep: remain time %ld.%09ld(sec)\n", (long)rem.tv_sec,
                                              (long)rem.tv_nsec);
                    req = rem; // call nanosleep() again with the remain time.
                } else {
                    // fatal error
                    LOG(ERROR) << "Failed to wait nanotime";
                    exit(0);
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &sleep_end_ts);

#ifdef VERBOSE_DEBUG
        DEBUG_PRINT("sleep_end_ts  : %010lu.%09lu\n", sleep_end_ts.tv_sec, sleep_end_ts.tv_nsec);
        if (sleep_end_ts.tv_nsec < sleep_start_ts.tv_nsec) {
            DEBUG_PRINT("start - end   : %10lu.%09lu\n", sleep_end_ts.tv_sec - sleep_start_ts.tv_sec - 1,
                        sleep_end_ts.tv_nsec + 1000000000 - sleep_start_ts.tv_nsec);
        } else {
            DEBUG_PRINT("start - end   : %10lu.%09lu\n", sleep_end_ts.tv_sec - sleep_start_ts.tv_sec,
                        sleep_end_ts.tv_nsec - sleep_start_ts.tv_nsec);
        }
#endif

        for (auto const &[i, mon] : monitors.mon | ranges::views::enumerate) {
            if (mon.status == MONITOR_DISABLE) {
                continue;
            }
            if (mon.status == MONITOR_ON) {
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                LOG(DEBUG) << fmt::format("[%d:%u:%u] start_ts: %010lu.%09lu\n", i, mon.tgid, mon.tid, start_ts.tv_sec,
                                          start_ts.tv_nsec);

#ifndef ONLY_CALCULATION
                /*stop target process group: send SIGSTOP */
                mon.run();
#endif
                /* read CBo values */
                uint64_t wb_cnt = 0;
                for (int j = 0; j < ncbo; j++) {
                    pmu.cbos[j].read_cbo_elems(&mon.after->cbos[j]);
                    wb_cnt += mon.after->cbos[j].llc_wb - mon.before->cbos[j].llc_wb;
                }
                LOG(ERROR) << fmt::format("[%d:%u:%u] LLC_WB = %" PRIu64 "\n", i, mon.tgid, mon.tid, wb_cnt);

                /* read CPU params */
                uint64_t cpus_dram_rds = 0;
                uint64_t target_l2stall = 0, target_llcmiss = 0, target_llchits = 0;
                for (int j = 0; j < ncpu; ++j) {
                    pmu.cpus[j].read_cpu_elems(&mon.after->cpus[j]);
                    cpus_dram_rds += mon.after->cpus[j].all_dram_rds - mon.before->cpus[j].all_dram_rds;
                }

                if (mon.num_of_region >= 2) {
                    /* read PEBS sample */
                    if (mon.pebs_ctx->read(mon.num_of_region, mon.region_info, &mon.after->pebs) < 0) {
                        LOG(ERROR) << fmt::format("[%d:%u:%u] Warning: Failed PEBS read\n", i, mon.tgid, mon.tid);
                    }
                    target_llcmiss = mon.after->pebs.llcmiss - mon.before->pebs.llcmiss;
                } else {
                    target_llcmiss =
                        mon.after->cpus[mon.cpu_core].cpu_llcl_miss - mon.before->cpus[mon.cpu_core].cpu_llcl_miss;
                }

                target_l2stall =
                    mon.after->cpus[mon.cpu_core].cpu_l2stall_t - mon.before->cpus[mon.cpu_core].cpu_l2stall_t;
                target_llchits =
                    mon.after->cpus[mon.cpu_core].cpu_llcl_hits - mon.before->cpus[mon.cpu_core].cpu_llcl_hits;

                if (cpus_dram_rds < target_llcmiss) {
                    LOG(DEBUG) << fmt::format(
                        "[%d:%u:%u]warning: target_llcmiss is more than cpus_dram_rds. target_llcmiss %ju, "
                        "cpus_dram_rds %ju\n",
                        i, mon.tgid, mon.tid, target_llcmiss, cpus_dram_rds);
                }
                uint64_t llcmiss_wb = 0;
                // To estimate the number of the writeback-involving LLC
                // misses of the CPU core (llcmiss_wb), the total number of
                // writebacks observed in L3 (wb_cnt) is devided
                // proportionally, according to the number of the ratio of
                // the LLC misses of the CPU core (target_llcmiss) to that
                // of the LLC misses of all the CPU cores and the
                // prefetchers (cpus_dram_rds).
                if (wb_cnt <= cpus_dram_rds && target_llcmiss <= cpus_dram_rds && cpus_dram_rds > 0) {
                    llcmiss_wb = wb_cnt * ((double)target_llcmiss / cpus_dram_rds);
                } else {
                    fprintf(stderr, "[%d:%u:%u]warning: wb_cnt %ju, target_llcmiss %ju, cpus_dram_rds %ju\n", i,
                            mon.tgid, mon.tid, wb_cnt, target_llcmiss, cpus_dram_rds);
                    llcmiss_wb = target_llcmiss;
                }

                uint64_t llcmiss_ro = 0;
                if (target_llcmiss < llcmiss_wb) {
                    LOG(ERROR) << fmt::format("[%d:%u:%u] cpus_dram_rds %lu, llcmiss_wb %lu, target_llcmiss %lu\n", i,
                                              mon.tgid, mon.tid, cpus_dram_rds, llcmiss_wb, target_llcmiss);
                    printf("!!!!llcmiss_ro is %lu!!!!!\n", llcmiss_ro);
                    llcmiss_wb = target_llcmiss;
                    llcmiss_ro = 0;
                } else {
                    llcmiss_ro = target_llcmiss - llcmiss_wb;
                }
                LOG(ERROR) << fmt::format("[%d:%u:%u]llcmiss_wb=%lu, llcmiss_ro=%lu\n", i, mon.tgid, mon.tid,
                                          llcmiss_wb, llcmiss_ro);

                uint64_t mastall_wb = 0;
                uint64_t mastall_ro = 0;
                // If both target_llchits and target_llcmiss are 0, it means that hit in L2.
                // Stall by LLC misses is 0.
                if (target_llchits || target_llcmiss) {
                    mastall_wb =
                        (double)(target_l2stall / frequency) *
                        ((double)(weight * llcmiss_wb) / (double)(target_llchits + (weight * target_llcmiss))) * 1000;
                    mastall_ro =
                        (double)(target_l2stall / frequency) *
                        ((double)(weight * llcmiss_ro) / (double)(target_llchits + (weight * target_llcmiss))) * 1000;
                }
                LOG(DEBUG) << fmt::format("l2stall=%" PRIu64 ", mastall_wb=%" PRIu64 ", mastall_ro=%" PRIu64
                                          ", target_llchits=%" PRIu64 ", target_llcmiss=%" PRIu64 ", weight=%lf\n",
                                          target_l2stall, mastall_wb, mastall_ro, target_llchits, target_llcmiss,
                                          weight);

                uint64_t ma_wb = (double)mastall_wb / latency;
                uint64_t ma_ro = (double)mastall_ro / latency;

                uint64_t emul_delay = 0;
                if (mon.num_of_region < 2) {
                    emul_delay = (double)(ma_ro) * (emul_nvm_lats[0].read - dram_latency) +
                                 (double)(ma_wb) * (emul_nvm_lats[0].write - dram_latency);
                } else { // Emulate Hybrid Memory
                    bool total_is_zero = (mon.after->pebs.total - mon.before->pebs.total) ? false : true;
                    double sample = 0;
                    double sample_prop = 0;
                    double sample_total = (double)(mon.after->pebs.total - mon.before->pebs.total);
                    if (total_is_zero) {
                        // If the total is 0, divide equally.
                        sample_prop = (double)1 / (double)mon.num_of_region;
                    }
                    LOG(DEBUG) << fmt::format("[%d:%u:%u] pebs: total=%lu, \n", i, mon.tgid, mon.tid,
                                              mon.after->pebs.total);
                    for (auto j = 0; j < mon.num_of_region; j++) {
                        if (!total_is_zero) {
                            sample = (double)(mon.after->pebs.sample[j] - mon.before->pebs.sample[j]);
                            sample_prop = sample / sample_total;
                        }
                        emul_delay += (double)(ma_ro)*sample_prop * (emul_nvm_lats[j].read - dram_latency) +
                                      (double)(ma_wb)*sample_prop * (emul_nvm_lats[j].write - dram_latency);
                        mon.before->pebs.sample[j] = mon.after->pebs.sample[j];
                        LOG(DEBUG) << fmt::format("[%d:%u:%u] pebs sample[%d]: =%lu, \n", i, mon.tgid, mon.tid, j,
                                                  mon.after->pebs.sample[j]);
                    }
                    mon.before->pebs.total = mon.after->pebs.total;
                }

                LOG(DEBUG) << fmt::format("ma_wb=%" PRIu64 ", ma_ro=%" PRIu64 ", delay=%" PRIu64 "\n", ma_wb, ma_ro,
                                          emul_delay);

                /* compensation of delay END(1) */
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
                LOG(DEBUG) << fmt::format("dif:%'12u\n", diff_nsec);

                uint64_t calibrated_delay = (diff_nsec > emul_delay) ? 0 : emul_delay - diff_nsec;
                // uint64_t calibrated_delay = emul_delay;
                mon.total_delay += (double)calibrated_delay / 1000000000;
                diff_nsec = 0;

#ifndef ONLY_CALCULATION
                /* insert emulated NVM latency */
                mon.injected_delay.tv_sec += (calibrated_delay / 1000000000);
                mon.injected_delay.tv_nsec += (calibrated_delay % 1000000000);
                LOG(DEBUG) << fmt::format("[%d:%u:%u]delay:%'10lu , total delay:%'lf\n", i, mon.tgid, mon.tid,
                                          calibrated_delay, mon.total_delay);
#endif
                auto swap = mon.before;
                mon.before = mon.after;
                mon.after = swap;

#ifndef ONLY_CALCULATION
                /* continue suspended processes: send SIGCONT */
                // unfreeze_counters_cbo_all(fds.msr[0]);
                // start_pmc(&fds, i);
                if (calibrated_delay == 0) {
                    mon.clear_time(&mon.wasted_delay);
                    mon.clear_time(&mon.injected_delay);
                    mon.run();
                }
#endif

            } else if (mon.status == MONITOR_OFF) {
                // Wasted epoch time
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                uint64_t sleep_diff = (sleep_end_ts.tv_sec - sleep_start_ts.tv_sec) * 1000000000 +
                                      (sleep_end_ts.tv_nsec - sleep_start_ts.tv_nsec);
                struct timespec sleep_time;
                sleep_time.tv_sec = sleep_diff / 1000000000;
                sleep_time.tv_nsec = sleep_diff % 1000000000;
                mon.wasted_delay.tv_sec += sleep_time.tv_sec;
                mon.wasted_delay.tv_nsec += sleep_time.tv_nsec;
                LOG(DEBUG) << fmt::format(
                    "[%d:%u:%u][OFF] total: %'lu | wasted : %'lu | waittime : %'lu | squabble : %'lu\n", i, mon.tgid,
                    mon.tid, mon.injected_delay.tv_nsec, mon.wasted_delay.tv_nsec, waittime.tv_nsec,
                    mon.squabble_delay.tv_nsec);
                if (monitors.check_continue(i, sleep_time)) {
                    mon.clear_time(&mon.wasted_delay);
                    mon.clear_time(&mon.injected_delay);
                    mon.run();
                }
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
            }

            if (mon.status == MONITOR_OFF && mon.injected_delay.tv_nsec != 0) {
                long remain_time = mon.injected_delay.tv_nsec - mon.wasted_delay.tv_nsec;
                /* do we need to get squabble time ? */
                if (mon.wasted_delay.tv_sec >= waittime.tv_sec && remain_time < waittime.tv_nsec) {
                    mon.squabble_delay.tv_nsec += remain_time;
                    if (mon.squabble_delay.tv_nsec < 40000000) {
                        LOG(DEBUG) << fmt::format(
                            "[SQ]total: %'lu | wasted : %'lu | waittime : %'lu | squabble : %'lu\n",
                            mon.injected_delay.tv_nsec, mon.wasted_delay.tv_nsec, waittime.tv_nsec,
                            mon.squabble_delay.tv_nsec);
                        mon.clear_time(&mon.wasted_delay);
                        mon.clear_time(&mon.injected_delay);
                        mon.run();
                    } else {
                        mon.injected_delay.tv_nsec += mon.squabble_delay.tv_nsec;
                        mon.clear_time(&mon.squabble_delay);
                    }
                }
            }
        } // End for-loop for all target processes
        if (monitors.check_all_terminated(tnum)) {
#ifdef VERBOSE_DEBUG
            LOG(DEBUG) << "All processes have already been terminated.\n";
#endif
            break;
        }
    } // End while-loop for emulation

    return 0;
}