#include "datetime/datetime_write.hpp"
#include "doris_rinex.hpp"
#include "doris_utils.hpp"
#include "geodesy/geodesy.hpp"
#include "geodesy/units.hpp"
#include "iers2010/iersc.hpp"
#include "iers2010/tropo.hpp"
#include "integrators.hpp"
#include "sp3/sp3.hpp"
#include "var_utils.hpp"
#include <cstdio>
#include "datetime/datetime_write.hpp"

// Standard gravitational parameters for Sun and Moon in [km^3 / sec^2]
double GMSun, GMMoon;

// usually using these datetimes ...
using Datetime = dso::datetime<dso::nanoseconds>;

struct TropoDetails {
  double Lhz, mfh;
  double Lwz, mfw;
  double sum() const noexcept {
    return Lhz * mfh + Lwz * mfw;
  }
};

struct SatBeacon {
  char id3c[3]; ///< beacons internal (3-char) id
  Datetime t;   ///< time
  double Diono; ///< ionospheric correction
  SatBeacon(const char *id_, const Datetime &t_, double Diono_) noexcept
      : t(t_), Diono(Diono_) {
    std::strncpy(id3c, id_, 3);
  }
  SatBeacon &operator=(const SatBeacon &sb) noexcept {
    std::strncpy(id3c, sb.id3c, 3);
    t = sb.t;
    Diono = sb.Diono;
    return *this;
  }
};

// hold satellite state & time
struct SatelliteState {
  Eigen::Matrix<double, 6, 1> state;
  double mjd_tai;

  Eigen::Matrix<double, 6, 1>
  celestial(const dso::EopLookUpTable &elut) const noexcept {
    // Terrestrial to Celestial transformation matrix and derivative for this
    // TAI
    Eigen::Matrix<double, 3, 3> dt2c;
    Eigen::Matrix<double, 3, 3> t2c = dso::itrs2gcrs(mjd_tai, elut, dt2c);

    // transform geocentric state to inertial to propagate orbit
    Eigen::Matrix<double, 6, 1> rcel;
    rcel.block<3, 1>(0, 0) = t2c * state.block<3, 1>(0, 0);
    rcel.block<3, 1>(3, 0) =
        t2c * state.block<3, 1>(3, 0) + dt2c * state.block<3, 1>(0, 0);

    return rcel;
  }

  int integrate(double mjd_target, dso::SGOde &integrator) noexcept {
    // Vector containing state + variational equations size: 6 + 6x6
    // Ref. Frame: inertial
    Eigen::Matrix<double, 6 + 6 * 6, 1> yPhi =
        Eigen::Matrix<double, 6 + 6 * 6, 1>::Zero();
    yPhi.block<6, 1>(0, 0) = celestial(integrator.params->eopLUT);
    {
      int k = 6;
      for (int col = 1; col < 7; col++)
        for (int row = 0; row < 6; row++)
          yPhi(k++) = (col - 1 == row) ? 1e0 : 0e0;
    }

    // t0 for variational equations (TAI)
    integrator.params->mjd_tai = mjd_tai;

    // target t for variational equations; seconds after t0
    double tout = (mjd_target - mjd_tai) * 86400e0;

    // set initial intergation flag
    integrator.flag() = 1;

    // keep solution here (celestial RF at tout)
    Eigen::VectorXd sol(6 + 6 * 6);

    // integrate (in inertial RF), from 0 to tout [sec]
    double tsec = 0e0;
    integrator.de(tsec, tout, yPhi, sol);

    // output epoch as datetime
    double tout_mjd = integrator.params->mjd_tai + tsec / 86400e0;

    // let's see were we are at
    if (std::abs(tout_mjd - mjd_target) > 1e-12) {
      fprintf(stderr,
              "ERROR wanted integration to %.9f and got up to %.9f, that is "
              "%.9f sec apart!\n",
              mjd_target, tout_mjd, (mjd_target - mjd_tai) * 86400e0);
      return 1;
    }

    // everything seems ok, update state and time
    mjd_tai = tout_mjd;

    // Terrestrial to Celestial transformation matrix and derivative for this
    // TAI
    Eigen::Matrix<double, 3, 3> dt2c;
    Eigen::Matrix<double, 3, 3> t2c =
        dso::itrs2gcrs(mjd_tai, integrator.params->eopLUT, dt2c);

    // transform inertial to geocentric
    state.block<3, 1>(0, 0) = t2c.transpose() * sol.block<3, 1>(0, 0);
    state.block<3, 1>(3, 0) = t2c.transpose() * sol.block<3, 1>(3, 0) +
                              dt2c.transpose() * sol.block<3, 1>(0, 0);

    return 0;
  }
};

int relativity_corrections(const Eigen::Matrix<double, 6, 1> &sv_state,
                           const Eigen::Matrix<double, 3, 1> &rbeacon,
                           double J2, double Re, double GM, double &Drel_c,
                           double &Drel_r) noexcept;
double ionospheric_correction(double Ls1, double Lu2, double Ls1_nominal,
                              double Lu2_nominal) noexcept;
Eigen::Matrix<double, 3, 1>
beacon_arp2ion(const Eigen::Matrix<double, 3, 1> &bxyz_arp,
               const dso::BeaconStation &beacon) noexcept;

// get the l1, l2 and f indexes off from a RINEX file (instance)
int get_rinex_indexes(const dso::DorisObsRinex &rnx, int &l1, int &l2,
                      int &f) noexcept;

Eigen::Matrix<double, 3, 1>
beacon_coordinates(const char *_4charid,
                   const std::vector<dso::BeaconCoordinates> &crdVec) noexcept;

int get_tropo(double mjd, const Eigen::Matrix<double, 3, 1> &bxyz,
              const Eigen::Matrix<double, 3, 1> &sxyz,
              const dso::Gpt3Grid &grid, TropoDetails &Dtrop) noexcept;

// get state of satellite at a given epoch
int get_satellite_sp3_state(const char *sp3, double mjd_tai,
                            Eigen::Matrix<double, 6, 1> &state,
                            double &mjd_state);

int main(int argc, char *argv[]) {
  // check input
  if (argc != 2) {
    fprintf(stderr, "USAGE: %s [YAML CONFIG]\n", argv[0]);
    return 1;
  }

  // resolve the yaml config file and get the root node
  const YAML::Node config = YAML::LoadFile(argv[1]);
  char buf[256];
  int error;
  
  // Load CSPICE/NAIF Kernels (2/3)
  // -------------------------------------------------------------------------
  if (dso::get_yaml_value_depth2(config, "naif-kernels", "spk", buf)) {
    fprintf(stderr, "ERROR failed to find spk kernel\n");
    return 1;
  }
  dso::cspice::load_if_unloaded_spk(buf);
  if (dso::get_yaml_value_depth2(config, "naif-kernels", "lsk", buf)) {
    fprintf(stderr, "ERROR failed to find spk kernel\n");
    return 1;
  }
  dso::cspice::load_if_unloaded_lsk(buf);

  // DORIS RINEX instance
  // Construct the DorisRinex instance rnx
  // -------------------------------------------------------------------------
  if (dso::get_yaml_value_depth2(config, "data", "doris-rinex", buf)) {
    fprintf(stderr, "ERROR. Failed parsing data/rinex file from YAML %s\n",
            argv[1]);
    return 1;
  }
  dso::DorisObsRinex rnx(buf);
#ifdef DEBUG
  rnx.print_metadata();
#endif
  
  // Initial Orbit
  // -------------------------------------------------------------------------
  // Intial satellite state, get it from the Sp3 using the RINEX's time of
  // first obs
  SatelliteState svState;
  {
    double rnx_first_obs = rnx.time_of_first_obs().as_mjd();
    dso::get_yaml_value_depth2(config, "data", "sp3", buf);
    if (get_satellite_sp3_state(buf, rnx_first_obs, svState.state,
                                svState.mjd_tai)) {
      return 1;
    }
  }

  // EOP Look Up Table
  // Parse the input EOP data file to create an EopLookUpTable eop_lut
  // -------------------------------------------------------------------------
  dso::EopLookUpTable eop_lut;
  if (dso::get_yaml_value_depth2(config, "eop-info", "eop-file", buf)) {
    fprintf(stderr,
            "ERROR. Failed parsing eop-info/eop-file file from YAML %s\n",
            argv[1]);
    return 1;
  } else {
    dso::EopFile eopin(buf);
    const int ref_mjd = rnx.ref_datetime().as_mjd();
    const int start = ref_mjd - 4;
    const int end = ref_mjd + 4;
    if (eopin.parse(start, end, eop_lut)) {
      fprintf(stderr, "ERROR. Failed collecting EOP data\n");
      return 1;
    }
  }

  // Gravity
  // -------------------------------------------------------------------------
  // parse degree and order and the requested gravity model into a
  // HarmonicCoeffs instance. Note that to compute potential we will need
  // Lagrange polynomials (later on)
  int degree, order;
  error = 0;
  error = dso::get_yaml_value_depth2<int>(config, "gravity", "degree", degree);
  error += dso::get_yaml_value_depth2<int>(config, "gravity", "order", order);
  dso::HarmonicCoeffs harmonics(degree);
  if (!error)
    error = dso::get_yaml_value_depth2(config, "gravity", "model", buf);
  if (!error)
    error = dso::parse_gravity_model(buf, degree, order, harmonics, true);
  if (error) {
    fprintf(stderr, "ERROR Failed handling gravity field model!\n");
    return 1;
  }

  // Troposphere
  // -------------------------------------------------------------------------
  // read-in the grid file. That is all for now
  dso::get_yaml_value_depth3(config, "troposphere", "gpt3", "grid", buf);
  dso::Gpt3Grid gpt3_grid(buf);
  
  // Station/Beacon coordinates
  // -------------------------------------------------------------------------
  // Get beacon coordinates from sinex file and extrapolate to RINEX ref. time
  // Result coordinates per beacon are stored in the beaconCrdVec vector.
  // Note that these poition vectors are w.r.t the beacon/antenna reference
  // point. When in actual processing, this has to be changed, if we are
  // considering iono-free analysis
  // -------------------------------------------------------------------------
  if (dso::get_yaml_value_depth2(config, "reference-frame",
                                 "station-coordinates", buf)) {
    fprintf(stderr,
            "ERROR. Failed parsing reference-frame/station-coordinates file "
            "from YAML %s\n",
            argv[1]);
    return 1;
  }
  std::vector<dso::BeaconCoordinates> beaconCrdVec;
  beaconCrdVec.reserve(70);
  if (extrapolate_sinex_coordinates(buf, rnx.stations(), rnx.ref_datetime(),
                                    beaconCrdVec, true)) {
    fprintf(stderr,
            "ERROR. Failed extracting/extrapolating beacon coordinates\n");
    return 1;
  }

  // Previous Observation relating Beacon/Satellite (to compute Ndop)
  // -------------------------------------------------------------------------
  std::vector<SatBeacon> prevec;
  prevec.reserve(beaconCrdVec.size());

  // Setup Integration Parameters for Orbit Integration
  // We will need the pck (SPICE) kernel for gravitational parameters of Sun
  // and Moon
  // -------------------------------------------------------------------------
  if (dso::get_yaml_value_depth2(config, "naif-kernels", "pck", buf)) {
    fprintf(stderr, "ERROR Failed locating NAIF pck kernel\n");
    return 1;
  }
  dso::IntegrationParameters IntegrationParams(degree, order, eop_lut,
                                               harmonics, buf);

  // Orbit Integrator
  // -------------------------------------------------------------------------
  // Setup an integrator, to extrapolate orbit with:
  // 1. Relative accuracy 1e-12
  // 2. Absolute accuracy 1e-12
  // 3. Num of Equations: 6 for state and 6*6 for variational equations
  dso::SGOde Integrator(dso::VariationalEquations, 6 + 6 * 6, 1e-12, 1e-12,
                        &IntegrationParams);

  // get the (RINEX) indexes for the observables we want
  int l1i, l2i, fi;
  if (get_rinex_indexes(rnx, l1i, l2i, fi))
    return 1;

  // Start RINEX data-block iteration
  // -------------------------------------------------------------------------
  // get an iterator to the RINEXs data blocks
  dso::RinexDataBlockIterator it(&rnx);

  // Some variables ...
  const double J2 = harmonics.J2();
  const double GM = harmonics.GM();
  const double Re = harmonics.Re();

  error = 0;
  int dummy_counter = 0;
  // for every new data block in the RINEX file (aka every epoch) ...
  while (!(error = it.next())) {
    // the current reference time for the L1 observation (corrected for
    // receiver clock offset)
    auto tl1 = it.corrected_l1_epoch();
        
    char dtbuf[64];
    dso::strftime_ymd_hmfs(it.cheader.m_epoch, dtbuf);
    printf("Processing observation block at %s (TAI)", dtbuf);

    // integrate orbit to here (TAI)
    // svState will contain satellite state for time tl1 in the terrestrial RF
    if (svState.integrate(tl1.as_mjd(), Integrator)) {
      fprintf(stderr, "ERROR. Failed to integrate orbit!\n");
      return 1;
    }
    printf(", SV at %.4f %.4f %.4f [km] ECEF\n", svState.state(0)*1e-3, svState.state(1)*1e-3, svState.state(2)*1e-3);

    // iterate through the observation set (aka the various beacons with
    // observations for current epoch)
    auto beaconobs = it.cblock.begin();
    while (beaconobs != it.cblock.end()) {

      // nominal frequencies for the beacon: s1_freq and u2_freq
      int k; // shift factor
      if (rnx.beacon_shift_factor(beaconobs->id(), k)) {
        fprintf(stderr, "Failed to find shift factor for beacon %.3s\n",
                beaconobs->id());
        return 1;
      }
      double fs1_nom, fu2_nom; // [Hz]
      dso::beacon_nominal_frequency(k, fs1_nom, fu2_nom);

      // what is the current beacon ?
      auto beacon_it = rnx.beacon_internal_id2BeaconStation(beaconobs->id());
      assert(beacon_it != rnx.stations().cend());

      printf("> Consuming new observation on beacon %.4s ...\n", beacon_it->m_station_id);

      // we are going to need the beacons ECEF coordinates (note that these
      // are antenna RP coordinates)
      const Eigen::Matrix<double, 3, 1> bxyz_arp =
          beacon_coordinates(beacon_it->m_station_id, beaconCrdVec);

      // get azimouth [rad], elevation [rad] and geometric distance [m]
      // (beacon to satellite)
      const Eigen::Matrix<double, 3, 1> r_enu =
          dso::car2top<dso::ellipsoid::grs80>(bxyz_arp,
                                              svState.state.block<3, 1>(0, 0));
      double az, el;
      const double rho = dso::top2dae(r_enu, az, el);

      // only process observations to elevation > 10 [deg]
      if (dso::rad2deg(el) < 10) {
        dso::strftime_ymd_hmfs(it.cheader.m_epoch, dtbuf);
        printf("\tSkipping observation to beacon %.4s because elevation is "
               "%.1f [deg] time: %s (TAI)\n",
               beacon_it->m_station_id, dso::rad2deg(el), dtbuf);
      } else {
        // elevation ok, continue processing

        // check if we already have a previous observation for this beacon
        auto pprev_obs = std::find_if(
            prevec.begin(), prevec.end(), [&](const SatBeacon &sb) {
              return (sb.id3c[0] == beaconobs->id()[0] &&
                      sb.id3c[1] == beaconobs->id()[1] &&
                      sb.id3c[2] == beaconobs->id()[2]);
            });

        // TODO what if it is the first ?

        // ionospheric correction (actually L_2GHz = L_2GHz + Diono)
        const double Diono = ionospheric_correction(
            beaconobs->m_values[l1i].m_value, beaconobs->m_values[l2i].m_value,
            fs1_nom, fu2_nom);

        // Iono-Free phase center w.r.t antenna RP, Cartesian ECEF
        const Eigen::Matrix<double, 3, 1> bxyz_ion =
            beacon_arp2ion(bxyz_arp, *beacon_it);

        // we need to find the true proper frequency of the receiver, f_rT [Hz]
        const double fs1_eT =
            fs1_nom * (1e0 + beaconobs->m_values[fi].m_value * 1e-11);

        // relativistic correction
        double Drel_c, Drel_r;
        relativity_corrections(svState.state, bxyz_ion, J2, Re, GM, Drel_c,
                               Drel_r);
        const double Drel = Drel_c + Drel_r;

        // tropospheric correction
        TropoDetails Dtropo;
        if (get_tropo(tl1.as_mjd(), bxyz_ion, svState.state.block<3, 1>(0, 0),
                      gpt3_grid, Dtropo)) {
          return 3;
        }

        // update previous measurement with this one
        if (pprev_obs == prevec.end()) {
          prevec.emplace_back(SatBeacon(beaconobs->id(), tl1, Diono));
        } else {
          *pprev_obs = SatBeacon(beaconobs->id(), tl1, Diono);
        }

        dso::strftime_ymd_hmfs(tl1, buf);
        printf("\t%s Elv:%.2f[deg] Rel:%+.9f[m] Trop:%+.9f[m] Iono:%+.9f[m] Fnom:%.3f[Hz] "
               "Ftrue:%.3f[Hz] R:%+.9f[km]\n",
               buf, dso::rad2deg(el), Drel, Dtropo.sum(),
               Diono * dso::DORIS_FREQ1_MHZ * 1e3, fs1_nom, fs1_eT, rho*1e-3);

      } // elevation > limit
      
      // netxt beacon observation block for this epoch
      ++beaconobs;
    } // for every beacon observation set in epoch
    
    if (++dummy_counter > 500) break;
  }   // for every new data-block/epoch in the RINEX file

  return 0;
}

int get_rinex_indexes(const dso::DorisObsRinex &rnx, int &l1_idx, int &l2_idx,
                      int &f_idx) noexcept {
  l1_idx = rnx.get_observation_code_index(
      dso::ObservationCode{dso::ObservationType::phase, 1});

  // index of the 400MHz phase measurement (need for iono-free reduction)
  l2_idx = rnx.get_observation_code_index(
      dso::ObservationCode{dso::ObservationType::phase, 2});

  // index of the F measurement (relative frequency offset)
  f_idx = rnx.get_observation_code_index(
      dso::ObservationCode{dso::ObservationType::frequency_offset});

  if (f_idx < 0 || l1_idx < 0 || l2_idx < 0) {
    fprintf(stderr,
            "[ERROR] Failed to find requested Observation Types in RINEX\'s "
            "observation "
            "types vector! (traceback: %s)\n",
            __func__);
    return 1;
  }

  return 0;
}

int get_satellite_sp3_state(const char *sp3fn, double mjd_tai,
                            Eigen::Matrix<double, 6, 1> &state,
                            double &mjd_state) {
  dso::sp3::SatelliteId sv;
  dso::Sp3DataBlock block, blockp;
  dso::Sp3c sp3(sp3fn);

  if (sp3.num_sats() == 1) {
    sv.set_id(sp3.sattellite_vector()[0].id);
  } else {
    fprintf(stderr, "More than one Satellites in sp3 file. This test program "
                    "is only meant to work with one.\n");
    return 1;
  }

  blockp.t = Datetime::max();

  int error = 0;
  while (!(error = sp3.get_next_data_block(sv, block))) {
    // check the heath status
    if (!block.flag.is_set(dso::Sp3Event::bad_abscent_position)) {
      if (block.t.as_mjd() > mjd_tai) {
        break;
      } else {
        blockp = block;
      }
    } 
  }
  if (error)
    return error;

  // accumulate state (m, m/sec), earth-fixed
  state << blockp.state[0] * 1e3, blockp.state[1] * 1e3, blockp.state[2] * 1e3,
      blockp.state[4] * 1e-1, blockp.state[5] * 1e-1, blockp.state[6] * 1e-1;

  // set time of state (TAI)
  mjd_state = blockp.t.as_mjd();

  return 0;
}

Eigen::Matrix<double, 3, 1>
beacon_coordinates(const char *_4charid,
                   const std::vector<dso::BeaconCoordinates> &crdVec) noexcept {
  const auto it = std::find_if(crdVec.cbegin(), crdVec.cend(),
                               [&](const dso::BeaconCoordinates &b) {
                                 return !std::strncmp(b.id, _4charid, 4);
                               });
  assert(it != crdVec.cend());
  double data[3] = {it->x, it->y, it->z};
  return Eigen::Matrix<double, 3, 1>(data);
}

int relativity_corrections(const Eigen::Matrix<double, 6, 1> &sv_state,
                           const Eigen::Matrix<double, 3, 1> &rbeacon,
                           double J2, double Re, double GM, double &Drel_c,
                           double &Drel_r) noexcept {
  // (J_2 augmented) potential at satellite (Larson et al, 2007)
  // satellite potential and velocity, state needed in ECEF here
  const double rs = sv_state.block<3, 1>(0, 0).norm();
  const double zs = sv_state(2);
  const double rs2 = rs * rs;
  const double Ur =
      -(GM / rs) * (1e0 - J2 * (Re / rs) * (Re / rs) *
                              ((3e0 * zs * zs - rs2) / (2e0 * rs2)));
  const double Vr2 = sv_state.block<3, 1>(3, 0).squaredNorm();

  // beacon potential and velocity
  const double rb = rbeacon.norm();
  const double zb = rbeacon(2);
  const double rb2 = rb * rb;
  const double Ue =
      -(GM / rb) * (1e0 - J2 * (Re / rb) * (Re / rb) *
                              ((3e0 * zb * zb - rb2) / (2e0 * rb2)));

  // relativity clock correction, Lemoine 2016
  Drel_c = (Ur - Ue + Vr2 / 2e0) / iers2010::C;

  // TODO relativity correction for travel time
  Drel_r = 0e0;

  return 0;
}

double ionospheric_correction(double Ls1, double Lu2, double Ls1_nominal,
                              double Lu2_nominal) noexcept {
  const double sroot_gamma = Ls1_nominal / Lu2_nominal;
  const double gamma = sroot_gamma * sroot_gamma;
  return (Ls1 - sroot_gamma * Lu2) / (gamma - 1e0);
}

Eigen::Matrix<double, 3, 1>
beacon_arp2ion(const Eigen::Matrix<double, 3, 1> &bxyz_arp,
               const dso::BeaconStation &beacon) noexcept {
  // transform cartesian to ellipsoidal
  auto lfh = dso::car2ell<dso::ellipsoid::grs80>(bxyz_arp);
  lfh(2) += beacon.iono_free_phase_center();
  return dso::ell2car<dso::ellipsoid::grs80>(lfh);
}

int get_tropo(double mjd, const Eigen::Matrix<double, 3, 1> &bxyz,
              const Eigen::Matrix<double, 3, 1> &sxyz,
              const dso::Gpt3Grid &grid, TropoDetails &Dtrop) noexcept {

  // fractional mjd to datetime
  double mjdi;
  double mjdf = std::modf(mjd, &mjdi);
  Datetime t(dso::modified_julian_day(static_cast<int>(mjdi)),
             dso::nanoseconds(static_cast<dso::nanoseconds::underlying_type>(
                 mjdf * 86400e0 * 1e9)));

  // ellipsoidal coordinates of the station; store them in an array
  Eigen::Matrix<double, 3, 1> bell = dso::car2ell<dso::ellipsoid::grs80>(bxyz);
  std::vector<std::array<double,3>> ellipsoidal(1,std::array<double,3>{bell(0), bell(1), bell(2)});

  // store results here
  std::vector<dso::gpt3_result> g3out;

  // call gpt3_fast
  if (dso::gpt3_fast(t, ellipsoidal, 1, grid, g3out)) {
    fprintf(stderr, "[ERROR] Failed to compute gpt3!\n");
    return 20;
  }
  // get azimouth [rad], elevation [rad] and geometric distance [m]
  // (beacon to satellite)
  const Eigen::Matrix<double, 3, 1> enu =
      dso::car2top<dso::ellipsoid::grs80>(bxyz, sxyz);
  double az, el;
  [[maybe_unused]] const double rho = dso::top2dae(enu, az, el);
  const double zd = iers2010::DPI / 2e0 - el;
  assert(zd >= 0e0 && zd <= iers2010::DPI / 2e0);

  // use VMF3 to compute
  double mfh, mfw;
  if (dso::vmf3(g3out[0].ah, g3out[0].aw, t, bell(1), bell(0), bell(2), mfh, mfw)) {
    fprintf(stderr, "Failed to compute VMF3\n");
    return 30;
  }

  // use refined saastamnoinen to compute the hydrostatic delay (zenith)
  const double zhd0 = dso::saasthyd(g3out[0].p, bell(1), bell(2));
  // apply VMF3 mapping function to compute hydrostatic delay at given zenith
  // const double Dtropo_hydrostatic = zhd0 * vmf3_res.mfh;

  // use Askne and Nordius to approximate wet dealy in zenith
  const double zwd0 = dso::asknewet(g3out[0].e, g3out[0].Tm, g3out[0].la);
  // apply VMF3 mapping function
  // const double Dtropo_wet = zwd0 * vmf3_res.mfw;

  Dtrop.Lhz = zhd0;
  Dtrop.mfh = mfh;
  Dtrop.Lwz = zwd0;
  Dtrop.mfw = mfw;

  return 0;
}

/*
double elevation(const Eigen::Matrix<double, 3, 1> &rsat,
                 const Eigen::Matrix<double, 3, 1> &rsta) noexcept {
  const Eigen::Matrix<double, 3, 1> lfh =
      dso::car2ell<dso::ellipsoid::grs80>(rsta);
  const double sf = std::sin(lfh(1));
  const double cf = std::cos(lfh(1));
  const double sl = std::sin(lfh(0));
  const double cl = std::cos(lfh(0));
  const double cwR[] = {sf*cl, sf*sl, -cf, -sl, cl, 0e0, cf*cl, cf*sl, sf};
                       
  Eigen::Matrix<double, 3, 3> R
  R << cwR[0], cwR[1], cwR[2], cwR[3], cwR[4], cwR[5], cwR[6], cwR[7], cwR[8];
  assert(R(0,0)== sf*cl && R(0,1)== sf*sl && R(0,2)==-cf);
  assert(R(1,0)==-sl && R(1,1)==cl && R(1,2)==0e0);
  assert(R(2,0)== cf*cl && R(2,1)== cf*sl && R(2,2)==sf);
  printf("Note-->topocentric matrix 2\n");
  for (int i=0; i<3; i++) {
    for (int j=0; j<3; j++) {
      printf(" %+.3f ", R(i,j));
    }
    printf("\n");
  }
  const Eigen::Matrix<double, 3, 1> rho = R * (rsat - rsta);
  printf("topocentric vector 2: %+.3f %+.3f %+.3f\n", rho(1), rho(0), rho(2));
  return std::asin(rho(2) / rho.norm());
}
*/
