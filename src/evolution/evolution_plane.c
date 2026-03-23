#include "evolution_plane.h"

void evolution_planner_init(void);
void evolution_corrector_init(void);
void evolution_verifier_init(void);
void evolution_evolver_init(void);

void evolution_plane_init(void)
{
    evolution_planner_init();
    evolution_corrector_init();
    evolution_verifier_init();
    evolution_evolver_init();
}
