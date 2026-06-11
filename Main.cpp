#include <stdlib.h>

#include <set> 
#include <numeric>
#include <optional>
#include <string>
#include <fstream>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "ortools/base/init_google.h"
#include "ortools/base/logging.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/util/sorted_interval_list.h"

//json
#include "json.hpp"



//COUPLE OF NOTES AND ASSUMPTIONS

// The priority of the meeting scales up the priority of preferences of the attendees, and schedules the meeting earlier if possible.
// This is currently just a base CPSAT implementation -- improvements are on the way in terms of MUS-finding, semantic preservation, weighted MUS-finding,
// other more efficient algorithms not fully integrated in most other libraries, possibly more. 
// Presumably, in the case of an intractible/no-solution input, you'll want to remove lower priority meetings from the input list.
// This is best done outside of the code below, as a wrapper. 
// On that note, for critical priority meetings, you might want to pre-set these at just the earliest hard-available, lowest-distance time for all required attendees, outside of the optimizer.
// example.json lists an example input to main. 
// There are a few soft constraints that might be mildly handy. I.E. try to choose rooms with the minimum excess capacity beyond what is needed for a meeting.
// Note that each additional constraint does contribute mildly to runtime, so I left some of these minor ones out. 

//self note delete later, verify weights

namespace operations_research {
namespace sat {

//SETUP CLASSES AND SUCH
enum class Priority { LOW = 0, MILD = 1, MEDIUM = 2, HIGH = 3, CRITICAL = 4 };
enum {ACCESSABILITY = 15, HEALTHISSUE = 10, ENVIRONMENTREQ = 5};

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
        int w = static_cast<int>(importance) * 10;
        if (has_accessibility_need) w += ACCESSABILITY;
        if (has_health_issue) w += HEALTHISSUE;
        if (has_env_request) w += ENVIRONMENTREQ;
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

//result of parsed input json
struct SchedulerInput 
{
    int total_slots;
    std::vector<Attendee> attendees;
    std::vector<Location> locations;
    std::vector<MeetingRequest> meetings;
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

//JSON PARSING RELATED STUFF
using json = nlohmann::json;


//users can specify with number 0-4 or with upper/lowercase level, returns Priority obj
static Priority ParsePriority(const json& j) 
{
    if (j.is_number_integer()) 
    {
        int p = j.get<int>();
        if (p < 0 || p > 4) 
            throw std::runtime_error("Priority int must be in [0, 4]");
        return static_cast<Priority>(p);
    }

    std::string s = j.get<std::string>();
    static const std::unordered_map<std::string, Priority> table = {
        {"LOW", Priority::LOW},
        {"MILD", Priority::MILD},
        {"MEDIUM", Priority::MEDIUM},
        {"HIGH", Priority::HIGH},
        {"CRITICAL", Priority::CRITICAL},
        {"low", Priority::LOW},
        {"mild", Priority::MILD},
        {"medium", Priority::MEDIUM},
        {"high", Priority::HIGH},
        {"critical", Priority::CRITICAL},
    };

    auto it = table.find(s);
    if (it == table.end()) 
        throw std::runtime_error("Unknown priority: " + s);

    return it->second;
}

static SchedulerInput ParseSchedulerInput(const json& j) 
{
    SchedulerInput input;
    input.total_slots = j.at("total_slots").get<int>();

    for (const auto& a : j.at("attendees")) 
    {
        input.attendees.push_back({
            a.at("name").get<std::string>(),
            ParsePriority(a.at("importance")),
            a.value("has_accessibility_need", false),
            a.value("has_health_issue", false),
            a.value("has_env_request", false),
            a.at("available_slots").get<std::vector<int>>(),
            a.value("preferred_slots", std::vector<int>{})
        });
    }

    for (const auto& l : j.at("locations")) 
    {
        input.locations.push_back({
            l.at("name").get<std::string>(),
            l.at("capacity").get<int>(),
            l.at("type").get<std::string>(),
            l.at("available_slots").get<std::vector<int>>(),
            l.value("distance_to_attendee", std::vector<int>{})
        });
    }

    for (const auto& m : j.at("meetings")) 
    {
        EventType event_type {
            m.at("event_type").at("name").get<std::string>(),
            ParsePriority(m.at("event_type").at("severity")),
            m.at("event_type").value("required_location_type", "")
        };

        std::optional<int> forced_location = std::nullopt;
        if (m.contains("forced_location") && !m.at("forced_location").is_null()) 
        {
            forced_location = m.at("forced_location").get<int>();
        }

        input.meetings.push_back({
            m.at("title").get<std::string>(),
            event_type,
            m.at("attendee_indices").get<std::vector<int>>(),
            m.at("duration_slots").get<int>(),
            forced_location
        });
    }

    return input;
}

static SchedulerInput LoadSchedulerInputFromArg(const std::string& arg) 
{
    json j;

    //Both of the following are accepted 
    //  ./main example.json
    //  ./main '{"total_slots":20,...}'
    if (!arg.empty() && arg.front() == '{') 
    {
        j = json::parse(arg);
    } 
    else //its a filename
    {
        std::ifstream in(arg);
        if (!in) 
            throw std::runtime_error("Could not open JSON input file: " + arg);
        in >> j;
    }

    return ParseSchedulerInput(j);
}


void ConstraintScheduler(const SchedulerInput& input) {
     
    const int kTotalSlots = input.total_slots;
    const std::vector<Attendee>& attendees = input.attendees;
    const std::vector<Location>& locations = input.locations;
    const std::vector<MeetingRequest>& meetings = input.meetings;

    //ACTUAL CPSAT CONSTRAINT STUFF BELOW HERE
    CpModelBuilder cp_model;
 
    int num_meetings = meetings.size();
    int num_locs = locations.size();

    //vector of start time for each meeting, and loc for each meeting
    std::vector<IntVar> start_slot(num_meetings);
    std::vector<IntVar> location_var(num_meetings);

    //optimize weight (lower better)
    LinearExpr objective;

    //for each meeting 
    for (int m = 0; m < num_meetings; ++m) 
    {
        const MeetingRequest& req = meetings[m];
    
        //intersection of hard-available slots
        std::vector<int> hard_slots = HardAvailableSlots(req, attendees, kTotalSlots);
    
        // slots that don't allow enough time for meeting durationare excluded 
        std::vector<int> filtered;
        for (int s : hard_slots)
        {
            if (s + req.duration_slots - 1 < kTotalSlots)
                filtered.push_back(s);
        }
        hard_slots = filtered;
    
        //slot domain is HARD CONSTRAINED to the hard-available slots
        Domain slot_domain = Domain::FromValues({hard_slots.begin(), hard_slots.end()});

        //variable for the start time of meeting m
        start_slot[m] = cp_model.NewIntVar(slot_domain).WithName("start_" + req.title);
    
        //LOCATION DOMAIN
        if (req.forced_location.has_value()) 
        {
            //this is a user override
            location_var[m] = cp_model.NewConstant(*req.forced_location).WithName("loc_" + req.title);
        } 
        else 
        {
            //THE FOLLOWING ARE CURRENTLY --HARD CONSTRAINTS-- on location
            // location type must match event's required type, if one is specified
            // location capacity >= number of attendees
            std::vector<int> valid_locs;
            for (int l = 0; l < num_locs; ++l) 
            {
                const Location& loc = locations[l];
                if (!req.event_type.required_location_type.empty() && loc.type != req.event_type.required_location_type)
                    continue;
                if (loc.capacity < req.attendee_indices.size())
                    continue;
                valid_locs.push_back(l);
            }
            Domain loc_domain = Domain::FromValues({valid_locs.begin(), valid_locs.end()});
            location_var[m] = cp_model.NewIntVar(loc_domain).WithName("loc_" + req.title);
        }

        // HARD CONSTRAINT room must be available for the full duration

        //for each location, a set of available times
        std::vector<std::set<int>> room_avail(num_locs);
        for (int l = 0; l < num_locs; ++l)
            room_avail[l] = {locations[l].available_slots.begin(), locations[l].available_slots.end()};
    
        // feasible (start, loc) pairs
        std::vector<std::vector<int64_t>> feasible_table;

        //from time 0 to last valid time
        for (int s = 0; s < kTotalSlots - req.duration_slots + 1; ++s) 
        {
            for (int l = 0; l < num_locs; ++l) 
            {
                bool room_ok = true;

                //check if available for the duration
                for (int d = 0; d < req.duration_slots; ++d) 
                {
                    if (room_avail[l].find(s + d) == room_avail[l].end()) 
                    {
                        room_ok = false; 
                        break;
                    }
                }   

                if (room_ok)
                    feasible_table.push_back({s, l});
            }
        }
        //meeting m must be something from the table of feasible options 
        //(this is essentially the "location/slot fits in valid times given loc duration" constraint)

        auto table1 = cp_model.AddAllowedAssignments({start_slot[m], location_var[m]});
        for (const auto& t : feasible_table) 
            table1.AddTuple(t);

        //SOFT CONSTRAINT - prefer slots preferred by high-weight attendees
        for (int ai : req.attendee_indices) 
        {
            const Attendee& att = attendees[ai];
            
            if (att.preferred_slots.empty()) 
                continue;
        
            Domain pref_domain = Domain::FromValues({att.preferred_slots.begin(), att.preferred_slots.end()});
            BoolVar not_preferred = cp_model.NewBoolVar();
            // not_preferred = 1  iff  start_slot not in pref_domain
            cp_model.AddLinearConstraint(start_slot[m], pref_domain).OnlyEnforceIf(not_preferred.Not());
            cp_model.AddLinearConstraint(start_slot[m], pref_domain.Complement()).OnlyEnforceIf(not_preferred);
            
            //penalty scaled by attendee importance and event severity
            int penalty = att.Weight() * (static_cast<int>(req.event_type.severity) + 1);
            objective += LinearExpr::Term(not_preferred, penalty);
        }

        //SOFT CONSTRAINT minimize total travel distance to location
        for (int ai : req.attendee_indices) 
        {
            //index l -> distance from location l to attendee ai, 999 if unlisted
            std::vector<int64_t> dist_table(num_locs);
            for (int l = 0; l < num_locs; ++l)
                //ideally shouldnt need this bounds check but just to be sure
                dist_table[l] = (ai < locations[l].distance_to_attendee.size()) ? locations[l].distance_to_attendee[ai] : 999;
        
            IntVar dist_var = cp_model.NewIntVar(Domain(0, 999)).WithName("dist_m"+std::to_string(m)+"_a"+ std::to_string(ai));
            
            //once solver chooses meeting location, set dist_var to distance from chosen location to this attendee
            cp_model.AddElement(location_var[m], dist_table, dist_var);

            objective += LinearExpr::Term(dist_var, 1);
        }

        // SOFT CONSRAINT higher-severity events get earlier slots 
        if (req.event_type.severity >= Priority::HIGH) 
        {
            int severity_w = static_cast<int>(req.event_type.severity) * 5;
            objective += LinearExpr::WeightedSum({start_slot[m]}, {severity_w});
        }
    }
        
    //HARD CONSTRAINT no room double-booking
    for (int l = 0; l < num_locs; ++l) 
    {
        std::vector<IntervalVar> room_intervals;
        for (int m = 0; m < num_meetings; ++m) 
        {
            BoolVar uses_this_room = cp_model.NewBoolVar().WithName("uses_loc" + std::to_string(l) + "_m" + std::to_string(m));
            cp_model.AddEquality(location_var[m], l).OnlyEnforceIf(uses_this_room);
            cp_model.AddNotEqual(location_var[m], l).OnlyEnforceIf(uses_this_room.Not());

            //interval exists if uses_this_room true
            //interval starts at start slot, duration is the duration of the meeting, ends at some point in the domain of totalslots (start + duration)
            IntervalVar interval = cp_model.NewOptionalIntervalVar(start_slot[m], 
                                                                    cp_model.NewConstant(meetings[m].duration_slots), 
                                                                    cp_model.NewIntVar(Domain(0, kTotalSlots)),
                                                                    uses_this_room)
                                                                    .WithName("iv_l" + std::to_string(l) + "_m" + std::to_string(m));
            room_intervals.push_back(interval);
        }
        cp_model.AddNoOverlap(room_intervals);
    }

    // HARD CONSTRAINT attendees cannot attend two meetings simultaneously
    for (int ai = 0; ai < attendees.size(); ++ai) 
    {
        std::vector<IntervalVar> att_intervals;
        for (int m = 0; m < num_meetings; ++m) 
        {
            bool in_meeting = false;
            //for each attendee, check if in a meeting
            for (int x : meetings[m].attendee_indices)
            {
                if (x == ai) 
                { 
                    in_meeting = true; 
                    break; 
                }
            }
            if (!in_meeting) continue;

            //build and push an interval
            IntervalVar iv = cp_model.NewIntervalVar(start_slot[m],
                                                        cp_model.NewConstant(meetings[m].duration_slots),
                                                        cp_model.NewIntVar(Domain(0, kTotalSlots))).WithName("att_iv_a" + std::to_string(ai) +"_m" + std::to_string(m));
            att_intervals.push_back(iv);
        }
        cp_model.AddNoOverlap(att_intervals);
    }

    cp_model.Minimize(objective);
    const CpSolverResponse response = Solve(cp_model.Build());

    //logging results
    if (response.status() == CpSolverStatus::OPTIMAL || response.status() == CpSolverStatus::FEASIBLE) 
    {
        LOG(INFO) << "=== Schedule found (objective = " << response.objective_value() << ") ===";
        if (response.status() == CpSolverStatus::OPTIMAL) LOG(INFO) << "OPTIMAL";
        if (response.status() == CpSolverStatus::FEASIBLE) LOG(INFO) << "FEASIBLE";
        for (int m = 0; m < num_meetings; ++m) 
        {
            int s = SolutionIntegerValue(response, start_slot[m]);
            int l = SolutionIntegerValue(response, location_var[m]);
            LOG(INFO) << "[" << meetings[m].title << "]"
                        << "  severity=" << static_cast<int>(meetings[m].event_type.severity)
                        << "  start_slot=" << s
                        << "  location=" << locations[l].name
                        << "  duration=" << meetings[m].duration_slots << " slot(s)";
            for (int ai : meetings[m].attendee_indices)
                LOG(INFO) << "  attendee: " << attendees[ai].name;
            
            LOG(INFO);
        }
    } 
    else 
    {
        LOG(INFO) << "No feasible schedule found.\n";
    }
        
}


}  // namespace sat
}  // namespace operations_research

int main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);

  operations_research::sat::SchedulerInput input = operations_research::sat::LoadSchedulerInputFromArg(argv[1]);
  operations_research::sat::ConstraintScheduler(input);
  return EXIT_SUCCESS;
}