/*
 * TODO:
 *  Figure out updaters
 *  Number of programmable HPCs
 *  Grouping algorithm, and handle if a group can't be scheduled? + unit tests
 *  There is a window of time where the child program may begin running, but the parent program
 *      has not yet enabled the perf events, meaning that this window of time will not be measured. Also,
 *      this window of time will be different for every iteration, affecting data. We need a synchronization technique
 *      to prevent the child program from starting until the parent has scheduled all events. This can be done
 *      by setting up a pipe between parent/child, and the child not calling execve until the parent gives it the OK.
 *  Refactor PmuEventParser to use a namespace
 *  Refactor handleCmdArgs to return a struct instead of modifying global vars.
 *  Take code out of main.cpp
 *  Refactor includes using include-what-you-use
 *  Fix problem where certain tests produce error logs when testing failures.
 */

#include <vector>
#include <iostream>
#include <getopt.h>
#include <cstring>
#include <string>
#include <memory>
#include <sys/wait.h>
#include <cstring>
#include "include/PmuEvent.h"
#include "include/PmuEventParser.h"
#include "include/EventGraph.h"
#include "include/Updaters.h"
#include "include/PmuGrouper.h"
#include "Perf.h"
#include "Logger.h"
#include "PfmLib.h"


static const std::string usageString = "Usage: bayesperf stat -e {events} {program}\n";
static std::vector<PmuEvent> events;

/*
 * cmd args for the program to run. This is stored as a char*[] instead of std::string because it will be
 * later fed into exec, which requires C-strings. argv already comes as C-strings, so it is easier to keep it
 * that way instead of converting between C-strings to std::string and back to C-string.
 *
 * This array's last element will be NULL, because it is required by exec.
 */
static std::unique_ptr<char*[]> programToRun;

void printUsage(){
    std::cout << usageString;
}

/*
 * Parses cmd args. This function will either call exit() and never return, or initialize the global variable
 * events.
 */
void handleCmdArgs(int argc, char* argv[]){
    int opt;

    if (strcmp(argv[1], "stat") != 0){
        printUsage();
        exit(0);
    }

    int optionsIndex = 0;
    static struct option long_options[] = {
            {"help",     no_argument, nullptr,  'h'},
            {"events",    required_argument,nullptr,  'e'},
            //last element of options array has to be filled with zeros
            {nullptr,      0,                 nullptr,    0}
    };

    /*we advance argv by 1 because we want to skip the bayesperf command. That is, if we call `bayesperf stat -e ...`,
    * we want to skip the "stat"
    * */
    while ((opt = getopt_long(argc - 1, &argv[1], "+e:", long_options, &optionsIndex)) != -1){
        switch(opt){
            case '?':
                exit(1); //getopt will automatically print the error to stderr
            case 'h':
                printUsage();
                exit(0);
            case 'e': {
                events = parseEvents(optarg);
                break;
            }
            default:
                printUsage();
                exit(0);
        }
    }

    optind += 1; //skip command to bayesperf

    int numCommandArgs = argc - optind;
    if (numCommandArgs == 0){
        std::cout << "Please input a program to execute\n";
        printUsage();
    }

    /*
     * NOTE: These args will be fed into exec. exec requires the last item of the array to be NULL. So we
     * need a +1 in size to add this final element.
     */
    programToRun = std::make_unique<char*[]>(numCommandArgs + 1);
    int i = 0;
    while (optind < argc){
        programToRun[i++] = argv[optind++];
    }
    programToRun[i] = nullptr;
}

EventGraph populateEventGraph(const std::vector<PmuEvent>& pmuEvents){
    EventGraph graph;
    for (const PmuEvent& event : pmuEvents){
        graph.addNode(event);
    }

    //TODO: read in statistical dependencies and add them.

    return graph;
}

EventGraph generateDebugGraph(){
    std::vector<PmuEvent> pmuEvents = {
            {"E1", PmuEvent::Type::SOFTWARE},
            {"E2", PmuEvent::Type::HARDWARE},
            {"E3", PmuEvent::Type::SOFTWARE},
            {"E4", PmuEvent::Type::HARDWARE},
            {"E5", PmuEvent::Type::SOFTWARE},
    };

    events = pmuEvents;

    EventGraph graph;
    for (const PmuEvent& event : pmuEvents){
        graph.addNode(event);
    }


//    TODO? UNSURE graph.addEdge(pmuEvents[0], pmuEvents[1],
//                  linearCorrectionUpdater(0.1, 0.11, 0.1)
//                  );

    return graph;
}

int reportError(const std::string& msg){
    int error = errno; //logging may modify errno, so we save it beforehand
    Logger::error(msg + ": " + strerror(errno));
    return error;
}

int main(int argc, char *argv[]) {
    PfmLib pfmlib; //instantiates pflmlib

    handleCmdArgs(argc, argv);
#ifdef BAYESPERF_DEBUG
    EventGraph graph = generateDebugGraph();
#else
    EventGraph graph = populateEventGraph(events);
#endif

    size_t groupSize = Perf::numProgrammableHPCs();
    std::vector<std::vector<PmuEvent>> groups = PmuGrouper::group(events, groupSize);

    pid_t pid = fork();
    if (pid < 0) {
        return reportError("fork()");
    }

    if (pid == 0) { //Child process
        if (execvp(programToRun[0], programToRun.get()) < 0) {
            return reportError("execvpe()");
        }
    }

    //Parent process
    Logger::debug("Child process created with pid " + std::to_string(pid));
    auto [fds, groupLeaderFds] = Perf::perfOpenEvents(groups, pid);
    Perf::enableEvents(groupLeaderFds); //Enabling only group leaders causes all events to be enabled

    int ret = waitpid(pid, nullptr, WNOHANG);
    while (ret <= 0){
        if (ret < 0){
            return reportError("waitpid()");
        }

        usleep(200000);
        for (int groupLeaderFd : groupLeaderFds){
            Perf::readSamplesForGroup(groupLeaderFd);
        }

        ret = waitpid(pid, nullptr, WNOHANG);
    }

    Perf::disableEvents(groupLeaderFds); //Disabling only group leaders causes all events to be disabled
    Perf::closeFds(fds);
    Logger::debug("Child process finalized executing");

    return 0;
}


/*
 * Hide idea of event graoh, instead just have an event manager
 * remove mean/std dev from event, have event manager store it
 * you can add events to the manager, and register connections between events, with a callback
 * when a new sample is obtained, push the sample to the manager
 * manager should update mean/variances, and then propagate changes
 */



