#include <stdlib.h>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "ortools/base/init_google.h"
#include "ortools/base/logging.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/util/sorted_interval_list.h"

namespace operations_research {
namespace sat {

//SETUP CLASSES AND SUCH
enum class Priority { LOW = 0, MILD = 1, MEDIUM = 2, HIGH = 3, CRITICAL = 4 };
enum {ACCESSABILITY = 15, HEALTHISSUE = 10, ENVIRONMENTREQ = 5}

struct Attendee {
    std::string name;
    Priority importance;
    bool has_accessibility_need;
    bool has_health_issue;
    bool has_env_request;
    
    // Availability as flat time-slot indices (timezone-normalised by caller)
    // eg 0 -> Monday 08:00, 1 -> Monday 08:30 
    
    //hard constraints 
    std::vector<int> available_slots;

    //these are soft
    std::vector<int> preferred_slots;
    
    // Composite weight: used to scale soft-constraint penalties.
    int Weight() const 
    {
        int w = importance * 10;
        if (has_accessibility_need) w += ACCESSABILITY;
        if (has_health_issue)       w += HEALTHISSUE;
        if (has_env_request)        w += ENVIRONMENTREQ;
        return w;
    }
};

struct Location 
{
    std::string name;
    int capacity;
    //eg "officeRoom", "auditorium", "virtual"
    std::string type; 
    // time slots when this room is free
    std::vector<int> available_slots; 
    //gonna assume we plug this in at some prepro step
    std::vector<int> distance_to_attendee;
};

struct EventType 
{
    std::string name;
    Priority severity;
    // "" means no restriction
    std::string required_location_type;
};

//a single meeting request to schedule.
struct MeetingRequest 
{
    std::string title;
    EventType event_type;
    //which attendees we're looking for
    std::vector<int> attendee_indices; 
    // how many consecutive slots needed
    int duration_slots;
    std::optional<int> forced_location;
};

//helper that collect slots that are available for all required attendees (preferences aside)
static std::vector<int> HardAvailableSlots(const MeetingRequest& req, const std::vector<Attendee>& attendees, int total_slots) 
{
    std::vector<bool> ok(total_slots, true);
  
    //use ANDs such that the ok vector is only those where all are available
    for (int ai : req.attendee_indices) 
    {
        std::vector<bool> att(total_slots, false);

        for (int s : attendees[ai].available_slots) 
            att[s] = true;
        for (int s = 0; s < total_slots; ++s)
            ok[s] = (ok[s] && att[s]);
    }

    //return those idxs
    std::vector<int> result;
    for (int s = 0; s < total_slots; ++s) 
        if (ok[s]) 
            result.push_back(s);
    return result;
}

//DUMMY CLAUDE-GENERATED DATA FOR NOW, PREPROCESSING PIPELINE LATER BASED ON REQS
void ConstraintScheduler() {
     
    // e.g. Mon–Fri 08:00–18:00 in 30-min blocks
    int kTotalSlots = 20; 
    
    std::vector<Attendee> attendees = {
        {"Alice (VP)",   Priority::CRITICAL, false, false, false,
        {0,1,2,3,4,5,6,7,8,9,10},   {2,3,4}},
        {"Bob (Manager)", Priority::HIGH,   false, true,  false,
        {2,3,4,5,6,7,8,9,10,11,12}, {4,5,6}},
        {"Carol (IC)",   Priority::MEDIUM,  true,  false, true,
        {0,1,2,3,4,5,6,7,8},        {0,1,2}},
        {"Dave (IC)",    Priority::LOW,     false, false, false,
        {5,6,7,8,9,10,11,12,13},    {7,8,9}},
    };
    
    std::vector<Location> locations = {
        {"Small Conf Room A", 4,  "conference_room", {0,1,2,3,4,5,6,7,8,9,10},
        {1, 3, 5, 2}},   // distance to each attendee
        {"Large Conf Room B", 12, "conference_room", {3,4,5,6,7,8,9,10,11,12},
        {4, 2, 1, 6}},
        {"Auditorium",        50, "auditorium",      {8,9,10,11,12,13,14},
        {2, 8, 3, 4}},
        {"Virtual / Teams",   999,"virtual",         /*always free*/ [&]{
        std::vector<int> v(kTotalSlots); std::iota(v.begin(), v.end(), 0);
        return v;}(),
        {0,0,0,0}},
    };
    
    std::vector<MeetingRequest> meetings = {
        {"Q2 All-Hands",
        {"All-Hands", Priority::CRITICAL, "auditorium"},
        {0,1,2,3}, 2, std::nullopt},
    
        {"1:1 Alice-Bob",
        {"1:1", Priority::HIGH, "conference_room"},
        {0,1},   1, std::nullopt},
    
        {"Carol Accessibility Check-in",
        {"Check-in", Priority::MEDIUM, "conference_room"},
        {0,2},   1, std::nullopt},
    
        {"Team Standup",
        {"Standup", Priority::MEDIUM, ""},   // any room type
        {0,1,2,3}, 1, std::nullopt},
    };

    //ACTUAL CPSAT SOLVING STUFF HERE
    

}

}  // namespace sat
}  // namespace operations_research

int main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
  operations_research::sat::ConstraintScheduler();
  return EXIT_SUCCESS;
}