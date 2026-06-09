#include <stdlib.h>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "ortools/base/init_google.h"
#include "ortools/base/logging.h"
#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/util/sorted_interval_list.h"



//COUPLE OF NOTES AND ASSUMPTIONS

// The priority of the meeting scales up the priority of preferences of the attendees, and schedules the meeting earlier if possible.
// This is currently just a base CPSAT implementation -- improvements are on the way in terms of MUS-finding, semantic preservation, weighted MUS-finding,
// other more efficient algorithms not fully integrated in most other libraries, possibly more. 
// Presumably, in the case of an intractible/no-solution input, you'll want to remove lower priority meetings from the input list.
// This is best done outside of the code below, as a wrapper. 
// On that note, example.json lists an example input to main. 
// There are a few constraints that might be mildly handy. I.E. try to choose rooms with the minimum excess capacity beyond what is needed for a meeting.

//self note delete later, check that the distance from attendee to location var is properly set 

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
    CpModelBuilder cp_model;
 
    int num_meetings = meetings.size();
    int num_locs = locations.size();

    //vector of start time for each meeting, and loc for each meeting
    std::vector<IntVar> start_slot(num_meetings);
    std::vector<IntVar> location_var(num_meetings);

    //optimize weight (lower better)
    LinearExpr objective;

    //for each meeting 
    for (int m = 0; m < num_meetings; ++m) {
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
            //this is a user overrid
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
        std::vector<std::vector<int>> feasible_table;

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
        cp_model.AddAllowedAssignments({start_slot[m], location_var[m]}, feasible_table);

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
            int penalty = att.Weight() * (req.event_type.severity + 1);
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

        // ---- Hard constraint: higher-severity events get earlier slots ----
        // Modelled as a soft penalty: penalise late start for critical events.
        if (req.event_type.severity >= Priority::HIGH) {
        int severity_w = static_cast<int>(req.event_type.severity) * 5;
        objective += LinearExpr::WeightedSum({start_slot[m]}, {severity_w});
        }
    }
        
    }


}

}  // namespace sat
}  // namespace operations_research

int main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, true);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
  operations_research::sat::ConstraintScheduler();
  return EXIT_SUCCESS;
}