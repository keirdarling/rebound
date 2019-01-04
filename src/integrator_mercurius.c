/**
 * @file    integrator_mercurius.c
 * @brief   MERCURIUS, a modified version of John Chambers' MERCURY algorithm
 *          using the IAS15 integrator and WHFast. At this point in time, 
 *          it is working but requires more testing. Use at your own risk!
 * @author  Hanno Rein
 * 
 * @section LICENSE
 * Copyright (c) 2017 Hanno Rein 
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "rebound.h"
#include "integrator.h"
#include "gravity.h"
#include "integrator_mercurius.h"
#include "integrator_ias15.h"
#include "integrator_whfast.h"
#include "collision.h"
#define MIN(a, b) ((a) > (b) ? (b) : (a))    ///< Returns the minimum of a and b
#define MAX(a, b) ((a) > (b) ? (a) : (b))    ///< Returns the maximum of a and b

double reb_integrator_mercurius_L_mercury(const struct reb_simulation* const r, double d, double dcrit){
    // This is the changeover function used by the Mercury integrator.
    double y = (d-0.1*dcrit)/(0.9*dcrit);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 1.;
    }else{
        return 10.*(y*y*y) - 15.*(y*y*y*y) + 6.*(y*y*y*y*y);
    }
}

static double f(double x){
    if (x<0) return 0;
    return exp(-1./x);
}

double reb_integrator_mercurius_L_infinity(const struct reb_simulation* const r, double d, double dcrit){
    // Infinitely differentiable function.
    double y = (d-0.1*dcrit)/(0.9*dcrit);
    if (y<0.){
        return 0.;
    }else if (y>1.){
        return 1.;
    }else{
        return f(y) /(f(y) + f(1.-y));
    }
}

static void reb_mercurius_encounterstep(struct reb_simulation* const r, const double _dt){
    // This function sets up the particle structures needed for IAS15 to run.
    // Only particles having a close encounter are integrated by IAS15.
    struct reb_simulation_integrator_mercurius* rim = &(r->ri_mercurius);
    struct reb_simulation_integrator_whfast* riw = &(r->ri_whfast);
    if (rim->encounterN<2){
        return; // If there are no particles (other than the star) having a close encounter, then there is nothing to do.
    }
    // Store number of (active) particles before the IAS15 integration
    // Used to restore everything after the step. 
    // Might be changed during a collision.
    rim->globalN = r->N;
    rim->globalNactive = r->N_active;

    // Allocate memory for the integration. Only resize if needed.
    if (rim->encounterAllocatedN<rim->encounterN){
        rim->encounterAllocatedN = rim->encounterN;
        rim->encounterParticles = realloc(rim->encounterParticles, sizeof(struct reb_particle)*rim->encounterN);
        rim->encounter_dcrit = realloc(rim->encounter_dcrit, sizeof(double)*rim->encounterN);
    }

    // Copy particles to temporary particle array.
    // Keeps track of number of active particles.
    r->N_active = 0;
    r->N = 0;
    for (unsigned int i=0; i<rim->globalN; i++){
        if(rim->encounterIndicies[i]>0){ // always include star in case of collisions
            rim->encounterParticles[r->N] = rim->p_hold[i];
            rim->encounterParticles[r->N].r = r->particles[i].r;
            rim->encounterParticles[r->N].ap = r->particles[i].ap;
            rim->encounterParticles[r->N].hash = r->particles[i].hash;
            rim->encounterParticles[r->N].lastcollision = r->particles[i].lastcollision;
            rim->encounter_dcrit[r->N] = rim->dcrit[i];
            r->N++;
            if ((int)i<rim->globalNactive){
                // The case globalNactive==-1 is handled below
                r->N_active++;
            }
        }
    }
    // manual set star particle
    rim->encounterParticles[0].m = r->particles[0].m;
    rim->encounterParticles[0].x = 0;
    rim->encounterParticles[0].y = 0;
    rim->encounterParticles[0].z = 0;
    rim->encounterParticles[0].vx = 0;
    rim->encounterParticles[0].vy = 0;
    rim->encounterParticles[0].vz = 0;
    if (rim->globalNactive==-1){
        r->N_active=-1;
    }

    // Swap pointers
    {
        struct reb_particle* temp = r->particles;
        r->particles = rim->encounterParticles;
        rim->encounterParticles = temp;
    }
    rim->mode = 1;
    
    // run
    const double old_dt = r->dt;
    const double old_t = r->t;
    double t_needed = r->t + _dt; 
        
    reb_integrator_ias15_reset(r);
    
    r->dt = 0.0001*_dt; // start with a small timestep.
    
    while(r->t < t_needed && fabs(r->dt/old_dt)>1e-14 ){
        reb_update_acceleration(r);
        reb_integrator_ias15_part2(r);

        reb_collision_search(r);
        if (r->t+r->dt >  t_needed){
            r->dt = t_needed-r->t;
        }
    }

    // Update particle coordinates in global arrays. 
    // If a collision occured, then encounterIndicies and
    // globalN will have changed.
    int k = 0;
    double totalm_new = 0;
    for (unsigned int i=0; i<rim->globalN; i++){
        totalm_new += r->particles[k].m;
        if(rim->encounterIndicies[i]>0){ // always include star in case of collisions
            riw->p_jh[i] = r->particles[k];
            // In case properties changed in a collision
            rim->encounterParticles[i].r = r->particles[k].r;
            rim->encounterParticles[i].ap = r->particles[k].ap;
            rim->encounterParticles[i].hash = r->particles[k].hash;
            rim->encounterParticles[i].lastcollision = r->particles[k].lastcollision;
            // Mass update is more complicated as it is in part done by the transformations.
            rim->encounterParticles[i].m = r->particles[k].m;
            k++;
        }
    }
    // Update total mass (0th particle is com)
    riw->p_jh[0].m = totalm_new;
    riw->p_jh[0].x = rim->p_hold[0].x;
    riw->p_jh[0].y = rim->p_hold[0].y;
    riw->p_jh[0].z = rim->p_hold[0].z;
    riw->p_jh[0].vx = rim->p_hold[0].vx;
    riw->p_jh[0].vy = rim->p_hold[0].vy;
    riw->p_jh[0].vz = rim->p_hold[0].vz;

    // Swap pointers
    {
        struct reb_particle* temp = r->particles;
        r->particles = rim->encounterParticles;
        rim->encounterParticles = temp;
    }
    // Reset constant for global particles
    r->N = rim->globalN;
    r->N_active = rim->globalNactive;
    r->t = old_t;
    r->dt = old_dt;
    rim->mode = 0;

}

static void reb_mercurius_predict_encounters(struct reb_simulation* const r){
    // This function predicts close encounters during the timestep
    // It makes use of the old and new position and velocities obtained
    // after the Kepler step.
    struct reb_simulation_integrator_mercurius* rim = &(r->ri_mercurius);
    struct reb_simulation_integrator_whfast* riw = &(r->ri_whfast);
    struct reb_particle* const p_hn = riw->p_jh;
    struct reb_particle* const p_ho = rim->p_hold;
    const double* const dcrit = rim->dcrit;
    const int N = r->N;
    const int N_active = r->N_active==-1?r->N:r->N_active;
    const double dt = r->dt;
    rim->encounterN = 1;
    rim->encounterIndicies[0] = 1;
    for (int i=1; i<N; i++){
        rim->encounterIndicies[i] = 0;
    }
    for (int i=0; i<N_active; i++){
        for (int j=i+1; j<N; j++){
            const double dxn = p_hn[i].x - p_hn[j].x;
            const double dyn = p_hn[i].y - p_hn[j].y;
            const double dzn = p_hn[i].z - p_hn[j].z;
            const double dvxn = p_hn[i].vx - p_hn[j].vx;
            const double dvyn = p_hn[i].vy - p_hn[j].vy;
            const double dvzn = p_hn[i].vz - p_hn[j].vz;
            const double rn = (dxn*dxn + dyn*dyn + dzn*dzn);
            const double dxo = p_ho[i].x - p_ho[j].x;
            const double dyo = p_ho[i].y - p_ho[j].y;
            const double dzo = p_ho[i].z - p_ho[j].z;
            const double dvxo = p_ho[i].vx - p_ho[j].vx;
            const double dvyo = p_ho[i].vy - p_ho[j].vy;
            const double dvzo = p_ho[i].vz - p_ho[j].vz;
            const double ro = (dxo*dxo + dyo*dyo + dzo*dzo);

            const double drndt = (dxn*dvxn+dyn*dvyn+dzn*dvzn)*2.;
            const double drodt = (dxo*dvxo+dyo*dvyo+dzo*dvzo)*2.;

            const double a = 6.*(ro-rn)+3.*dt*(drodt+drndt); 
            const double b = 6.*(rn-ro)-2.*dt*(2.*drodt+drndt); 
            const double c = dt*drodt; 

            double rmin = MIN(rn,ro);

            const double s = b*b-4.*a*c;
            const double sr = sqrt(MAX(0.,s));
            const double tmin1 = (-b + sr)/(2.*a); 
            const double tmin2 = (-b - sr)/(2.*a); 
            if (tmin1>0. && tmin1<1.){
                const double rmin1 = (1.-tmin1)*(1.-tmin1)*(1.+2.*tmin1)*ro
                                     + tmin1*tmin1*(3.-2.*tmin1)*rn
                                     + tmin1*(1.-tmin1)*(1.-tmin1)*dt*drodt
                                     - tmin1*tmin1*(1.-tmin1)*dt*drndt;
                rmin = MIN(MAX(rmin1,0.),rmin);
            }
            if (tmin2>0. && tmin2<1.){
                const double rmin2 = (1.-tmin2)*(1.-tmin2)*(1.+2.*tmin2)*ro
                                     + tmin2*tmin2*(3.-2.*tmin2)*rn
                                     + tmin2*(1.-tmin2)*(1.-tmin2)*dt*drodt
                                     - tmin2*tmin2*(1.-tmin2)*dt*drndt;
                rmin = MIN(MAX(rmin2,0.),rmin);
            }

            if (sqrt(rmin)< 1.1*MAX(dcrit[i],dcrit[j])){
                if (rim->encounterIndicies[i]==0){
                    rim->encounterIndicies[i] = i;
                    rim->encounterN++;
                }
                if (rim->encounterIndicies[j]==0){
                    rim->encounterIndicies[j] = j;
                    rim->encounterN++;
                }
            }
        }
    }
}

void reb_integrator_mercurius_part1(struct reb_simulation* r){
    if (r->var_config_N){
        reb_warning(r,"Mercurius does not work with variational equations.");
    }
    
    struct reb_particle* restrict const particles = r->particles;
    struct reb_simulation_integrator_mercurius* const rim = &(r->ri_mercurius);
    struct reb_simulation_integrator_whfast* const riw = &(r->ri_whfast);
    const int N = r->N;
   
    
    if (rim->dcrit_allocatedN<N){
        rim->dcrit_allocatedN = N;
        rim->dcrit              = realloc(rim->dcrit, sizeof(double)*N);
        rim->recalculate_dcrit_this_timestep        = 1;
    }
    if (rim->allocatedN<N){
        // These arrays are only used within one timestep. 
        // Can be recreated without loosing bit-wise reproducibility
        rim->allocatedN = N;
        rim->encounterIndicies  = realloc(rim->encounterIndicies, sizeof(unsigned int)*N);
        rim->p_hold             = realloc(rim->p_hold,sizeof(struct reb_particle)*N);
    }
    if (riw->allocated_N<N){
        // Heliocentric coordinates. Stored in WHFast struct.
        riw->allocated_N = N;
        riw->p_jh = realloc(riw->p_jh,sizeof(struct reb_particle)*N);
        rim->recalculate_coordinates_this_timestep = 1;
    }
    if (rim->safe_mode || rim->recalculate_coordinates_this_timestep){
        rim->recalculate_coordinates_this_timestep = 0;
        if (rim->is_synchronized==0){
            reb_integrator_mercurius_synchronize(r);
            reb_warning(r,"MERCURIUS: Recalculating heliocentric coordinates but pos/vel were not synchronized before.");
        }
        reb_transformations_inertial_to_democraticheliocentric_posvel(particles, riw->p_jh, N);
    }

    if (rim->recalculate_dcrit_this_timestep){
        rim->recalculate_dcrit_this_timestep = 0;
        if (rim->is_synchronized==0){
            reb_integrator_mercurius_synchronize(r);
            reb_warning(r,"MERCURIUS: Recalculating dcrit but pos/vel were not synchronized before.");
        }
        rim->dcrit[0] = 2.*r->particles[0].r; // central object only uses physical radius
        const double m0 = r->particles[0].m;
        for (int i=1;i<N;i++){
            const double dx  = riw->p_jh[i].x;
            const double dy  = riw->p_jh[i].y;
            const double dz  = riw->p_jh[i].z;
            const double dvx = r->particles[i].vx - r->particles[0].vx; 
            const double dvy = r->particles[i].vy - r->particles[0].vy; 
            const double dvz = r->particles[i].vz - r->particles[0].vz; 
            const double _r = sqrt(dx*dx + dy*dy + dz*dz);
            const double v2 = dvx*dvx + dvy*dvy + dvz*dvz;

            const double GM = r->G*(m0+r->particles[i].m);
            const double a = GM*_r / (2.*GM - _r*v2);
            const double vc = sqrt(GM/fabs(a));
            double dcrit = 0;
            // Criteria 1: average velocity
            dcrit = MAX(dcrit, vc*0.4*r->dt);
            // Criteria 2: current velocity
            dcrit = MAX(dcrit, sqrt(v2)*0.4*r->dt);
            // Criteria 3: Hill radius
            dcrit = MAX(dcrit, rim->hillfac*a*pow(r->particles[i].m/(3.*r->particles[0].m),1./3.));
            // Criteria 4: physical radius
            dcrit = MAX(dcrit, 2.*r->particles[i].r);

            rim->dcrit[i] = dcrit;
        }
    }
    
    // Force use of democratic heliocentric coordinates.
    riw->coordinates = REB_WHFAST_COORDINATES_DEMOCRATICHELIOCENTRIC;
    
    // Calculate gravity with special function
    if (r->gravity != REB_GRAVITY_BASIC && r->gravity != REB_GRAVITY_MERCURIUS){
        reb_warning(r,"Mercurius has it's own gravity routine. Gravity routine set by the user will be ignored.");
    }
    r->gravity = REB_GRAVITY_MERCURIUS;
    rim->mode = 0;
    
    if (rim->L == NULL){
        // Setting default switching function
        rim->L = reb_integrator_mercurius_L_mercury;
    }
}


void reb_integrator_mercurius_part2(struct reb_simulation* const r){
    struct reb_simulation_integrator_mercurius* const rim = &(r->ri_mercurius);
    struct reb_simulation_integrator_whfast* const riw = &(r->ri_whfast);
    struct reb_particle* restrict const particles = r->particles;
    const int N = r->N;
   
    if (rim->is_synchronized){
        reb_whfast_interaction_step(r,r->dt/2.);
    }else{
        reb_whfast_interaction_step(r,r->dt);
    }
    reb_whfast_jump_step(r,r->dt/2.);
   
    reb_whfast_com_step(r,r->dt);
    
    memcpy(rim->p_hold,riw->p_jh,N*sizeof(struct reb_particle));
    reb_whfast_kepler_step(r,r->dt);
    
    reb_mercurius_predict_encounters(r);
   
    reb_mercurius_encounterstep(r,r->dt);
    
    reb_whfast_jump_step(r,r->dt/2.);
        
    reb_transformations_democraticheliocentric_to_inertial_posvel(particles, riw->p_jh, N);
    
    rim->is_synchronized = 0;
    if (rim->safe_mode){
        reb_integrator_mercurius_synchronize(r);
    }

    r->t+=r->dt;
    r->dt_last_done = r->dt;
}

void reb_integrator_mercurius_synchronize(struct reb_simulation* r){
    struct reb_simulation_integrator_mercurius* const rim = &(r->ri_mercurius);
    if (rim->is_synchronized == 0){
        struct reb_simulation_integrator_whfast* const riw = &(r->ri_whfast);
        struct reb_particle* restrict const particles = r->particles;
        const int N = r->N;
        struct reb_particle* sync_ph = NULL;
        if (rim->keep_unsynchronized){
            sync_ph = malloc(sizeof(struct reb_particle)*r->N);
            memcpy(sync_ph,riw->p_jh,r->N*sizeof(struct reb_particle));
        }
    
        r->gravity = REB_GRAVITY_MERCURIUS; // needed for SimulationArchive
        riw->coordinates = REB_WHFAST_COORDINATES_DEMOCRATICHELIOCENTRIC;
        rim->mode = 0;
        reb_calculate_acceleration(r);
        reb_whfast_interaction_step(r,r->dt/2.);
        
        reb_transformations_democraticheliocentric_to_inertial_posvel(particles, riw->p_jh, N);

        if (rim->keep_unsynchronized){
            memcpy(riw->p_jh,sync_ph,r->N*sizeof(struct reb_particle));
            free(sync_ph);
        }else{
            rim->is_synchronized = 1;
        }
    }
}

void reb_integrator_mercurius_reset(struct reb_simulation* r){
    r->ri_mercurius.L = NULL;
    r->ri_mercurius.mode = 0;
    r->ri_mercurius.encounterN = 0;
    r->ri_mercurius.globalN = 0;
    r->ri_mercurius.globalNactive = 0;
    r->ri_mercurius.hillfac = 3;
    r->ri_mercurius.keep_unsynchronized = 0;
    r->ri_mercurius.recalculate_coordinates_this_timestep = 0;
    // Arrays
    r->ri_mercurius.encounterAllocatedN = 0;
    free(r->ri_mercurius.encounterParticles);
    r->ri_mercurius.encounterParticles = NULL;
    free(r->ri_mercurius.encounter_dcrit);
    r->ri_mercurius.encounter_dcrit = NULL;

    r->ri_mercurius.allocatedN = 0;
    free(r->ri_mercurius.p_hold);
    r->ri_mercurius.p_hold = NULL;
    free(r->ri_mercurius.encounterIndicies);
    r->ri_mercurius.encounterIndicies = NULL;
    r->ri_mercurius.dcrit_allocatedN = 0;
    free(r->ri_mercurius.dcrit);
    r->ri_mercurius.dcrit = NULL;
}

