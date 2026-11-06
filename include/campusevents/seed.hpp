#pragma once
// Deterministic sample-data generator used for the demo and the screenshots.
// It goes through EventService, so every row is created by the same rules as real requests.
#include "campusevents/service.hpp"

namespace ce {

struct SeedSummary {
    int venues = 0;
    int eventsCreated = 0;
    int eventsRefusedClash = 0;  // attempted bookings the venue-clash rule rejected
    int registrationsConfirmed = 0;
    int registrationsWaiting = 0;
    int registrationsRefusedStudentClash = 0;
    int checkIns = 0;
    int promotions = 0;  // waiting-list promotions triggered by cancellations
};

SeedSummary seedDemoData(EventService& service, unsigned randomSeed = 2026);

}  // namespace ce
