/*
  Copyright (C) 2010-2018 The ESPResSo project
  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
     Max-Planck-Institute for Polymer Research, Theory Group

  This file is part of ESPResSo.

  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/** \file
 *  Implementation of dpd.hpp.
 */
#include "dpd.hpp"

#ifdef DPD

#include "communication.hpp"
#include "random.hpp"
#include "short_range_loop.hpp"
#include "thermostat.hpp"

#include "utils/NoOp.hpp"

using Utils::Vector3d;

void dpd_heat_up() {
  double pref_scale = sqrt(3);
  dpd_update_params(pref_scale);
}

void dpd_cool_down() {
  double pref_scale = 1.0 / sqrt(3);
  dpd_update_params(pref_scale);
}

int dpd_set_params(int part_type_a, int part_type_b, double gamma, double r_c,
                   int wf, double tgamma, double tr_c, int twf) {
  IA_parameters *data = get_ia_param_safe(part_type_a, part_type_b);

  data->dpd_gamma = gamma;
  data->dpd_r_cut = r_c;
  data->dpd_wf = wf;
  data->dpd_pref2 = sqrt(24.0 * temperature * gamma / time_step);
  data->dpd_tgamma = tgamma;
  data->dpd_tr_cut = tr_c;
  data->dpd_twf = twf;
  data->dpd_pref4 = sqrt(24.0 * temperature * tgamma / time_step);

  /* Only make active if the DPD thermostat is
     activated, otherwise it will by activated
     by dpd_init() on thermostat change.
  */
  if (thermo_switch & THERMO_DPD) {
    data->dpd_pref1 = gamma / time_step;
    data->dpd_pref3 = tgamma / time_step;
  } else {
    data->dpd_pref1 = 0.0;
    data->dpd_pref3 = 0.0;
  }

  /* broadcast interaction parameters */
  mpi_bcast_ia_params(part_type_a, part_type_b);

  return ES_OK;
}

void dpd_init() {
  for (int type_a = 0; type_a < max_seen_particle_type; type_a++) {
    for (int type_b = 0; type_b < max_seen_particle_type; type_b++) {
      auto data = get_ia_param(type_a, type_b);
      if ((data->dpd_r_cut != 0) || (data->dpd_tr_cut != 0)) {
        data->dpd_pref1 = data->dpd_gamma / time_step;
        data->dpd_pref2 =
            sqrt(24.0 * temperature * data->dpd_gamma / time_step);
        data->dpd_pref3 = data->dpd_tgamma / time_step;
        data->dpd_pref4 =
            sqrt(24.0 * temperature * data->dpd_tgamma / time_step);
      }
    }
  }
}

void dpd_switch_off() {
  for (int type_a = 0; type_a < max_seen_particle_type; type_a++) {
    for (int type_b = 0; type_b < max_seen_particle_type; type_b++) {
      auto data = get_ia_param(type_a, type_b);
      data->dpd_pref1 = data->dpd_pref3 = 0.0;
    }
  }
}

void dpd_update_params(double pref_scale) {
  int type_a, type_b;
  IA_parameters *data;

  for (type_a = 0; type_a < max_seen_particle_type; type_a++) {
    for (type_b = 0; type_b < max_seen_particle_type; type_b++) {
      data = get_ia_param(type_a, type_b);
      if ((data->dpd_r_cut != 0) || (data->dpd_tr_cut != 0)) {
        data->dpd_pref2 *= pref_scale;
        data->dpd_pref4 *= pref_scale;
      }
    }
  }
}

static double weight(int type, double r_cut, double dist_inv) {
  if (type == 0) {
    return dist_inv;
  }
  return dist_inv - 1.0 / r_cut;
}

Vector3d dpd_pair_force(Particle const *p1, Particle const *p2,
                        IA_parameters *ia_params, double const *d, double dist,
                        double dist2, bool include_noise) {
  Vector3d f{};

  auto const dist_inv = 1.0 / dist;

  if ((dist < ia_params->dpd_r_cut) && (ia_params->dpd_pref1 > 0.0)) {
    auto const omega =
        weight(ia_params->dpd_wf, ia_params->dpd_r_cut, dist_inv);
    auto const omega2 = Utils::sqr(omega);
    // DPD part
    // friction force prefactor
    double vel12_dot_d12 = 0.0;
    for (int j = 0; j < 3; j++)
      vel12_dot_d12 += (p1->m.v[j] - p2->m.v[j]) * d[j];
    auto const friction =
        ia_params->dpd_pref1 * omega2 * vel12_dot_d12 * time_step;
    // random force prefactor
    double noise;
    if (ia_params->dpd_pref2 > 0.0 && include_noise == true) {
      noise = ia_params->dpd_pref2 * omega * (d_random() - 0.5);
    } else {
      noise = 0.0;
    }
    for (int j = 0; j < 3; j++) {
      f[j] += (noise - friction) * d[j];
    }
  }
  // DPD2 part
  if ((dist < ia_params->dpd_tr_cut) && (ia_params->dpd_pref3 > 0.0)) {
    auto const omega =
        weight(ia_params->dpd_twf, ia_params->dpd_tr_cut, dist_inv);
    auto const omega2 = Utils::sqr(omega);

    double P_times_dist_sqr[3][3] = {{dist2, 0, 0},
                                     {0, dist2, 0},
                                     {0, 0, dist2}},
           noise_vec[3];
    for (int i = 0; i < 3; i++) {
      // noise vector
      if (ia_params->dpd_pref2 > 0.0 && include_noise == true) {
        noise_vec[i] = d_random() - 0.5;
      } else {
        noise_vec[i] = 0.0;
      }
      // Projection Matrix
      for (int j = 0; j < 3; j++) {
        P_times_dist_sqr[i][j] -= d[i] * d[j];
      }
    }

    double f_D[3], f_R[3];
    for (int i = 0; i < 3; i++) {
      // Damping force
      f_D[i] = 0;
      // Random force
      f_R[i] = 0;
      for (int j = 0; j < 3; j++) {
        f_D[i] += P_times_dist_sqr[i][j] * (p1->m.v[j] - p2->m.v[j]);
        f_R[i] += P_times_dist_sqr[i][j] * noise_vec[j];
      }
      // NOTE: velocity are scaled with time_step
      f_D[i] *= ia_params->dpd_pref3 * omega2 * time_step;
      // NOTE: noise force scales with 1/sqrt(time_step
      f_R[i] *= ia_params->dpd_pref4 * omega * dist_inv;
    }
    for (int j = 0; j < 3; j++) {
      f[j] += f_R[j] - f_D[j];
    }
  }

  return f;
}

Utils::Vector9d dpd_stress() {
  using Utils::Vector9d;

  Vector9d dpd_stress{};

  if (max_cut > 0) {
    short_range_loop(
        Utils::NoOp{}, [&dpd_stress](Particle &p1, Particle &p2, Distance &d) {
          IA_parameters *ia_params = get_ia_param(p1.p.type, p2.p.type);
          bool include_noise = false;
          auto const f = dpd_pair_force(&p1, &p2, ia_params, d.vec21.data(),
                                        sqrt(d.dist2), d.dist2, include_noise);
          const Vector3d &r = d.vec21;
          dpd_stress += Vector9d{r[0] * f[0], r[0] * f[1], r[0] * f[2],
                                 r[1] * f[0], r[1] * f[1], r[1] * f[2],
                                 r[2] * f[0], r[2] * f[1], r[2] * f[2]};
        });
  }
  dpd_stress /= (box_l[0] * box_l[1] * box_l[2]);
  return dpd_stress;
}
#endif
