#include "evolution_plane.h"
#include "evolution_snapshot.h"
#include "evolution_metrics.h"

void evolution_planner_init(void);
void evolution_corrector_init(void);
void evolution_verifier_init(void);
void evolution_evolver_init(void);

void evolution_plane_init(void)
{
    evolution_snapshot_init();
    evolution_metrics_init();
    (void)evolution_snapshot_register_current("boot-baseline");
    evolution_planner_init();
    evolution_corrector_init();
    evolution_verifier_init();
    evolution_evolver_init();
}
