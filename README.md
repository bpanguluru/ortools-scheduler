Use init_run.sh first to allow dependencies and all to compile.
Use testing_rn.sh afterward for some slight time-saving. 

##Some notes and assumptions

- The priority of the meeting scales up the priority of preferences of the attendees, and schedules the meeting earlier if possible.
- This is currently just a base CPSAT implementation -- improvements are on the way in terms of MUS-finding, semantic preservation, weighted MUS-finding, other more efficient algorithms not fully integrated in most other libraries, possibly more. 
- Presumably, in the case of an intractible/no-solution input, you'll want to remove lower priority meetings from the input list. This is best done outside of the code below, as a wrapper. 
- On that note, for critical priority meetings, you might want to pre-set these at just the earliest hard-available, lowest-distance time for all required attendees, outside of the optimizer.
- example.json lists an example input to main. (tba)
- There are a few soft constraints that might be mildly handy. I.E. try to choose rooms with the minimum excess capacity beyond what is needed for a meeting.
- Note that each additional constraint does contribute mildly to runtime, so I left some of these minor ones out. 