#include "astrodynamics.hpp"
#include "atmosphere.hpp"
#include "datetime/datetime_write.hpp"
#include "egravity.hpp"
#include "eigen3/Eigen/Eigen"
#include "geodesy/units.hpp"
#include "geodesy/geodesy.hpp"
#include "icgemio.hpp"
#include "iers2010/iau.hpp"
#include "iers2010/iers2010.hpp"
#include "iers_bulletin.hpp"
#include "integrators.hpp"
#include "planetpos.hpp"
#include "sp3/sp3.hpp"
#include <cassert>
#include <cstdio>
#include <charconv>
#include "yaml-cpp/yaml.h"

using dso::sp3::SatelliteId;

const int Integrate = true;
const int include_third_body = true;
const int include_drag = false;
const int include_srp = false;

// approximate number of data points in a bulletin B file (disregarding
// preliminery values)
constexpr const int BBSZ = 40;

// count calls to VE
static unsigned call_nr = 0;

// Radiation pressure scale coefficient for H2C
// see https://ids-doris.org/documents/BC/satellites/DORISSatelliteModels.pdf
constexpr const double Cr_H2C = 0.88e0;
constexpr const double Mass_H2C = 1677e0; // [kg] total mass of H2C
constexpr const double Area_H2C = 39.71e0; // [m^2]
constexpr const double CD = 2.25e0; // drag coefficient in range [1.5 - 3]

// Standard gravitational parameters for Sun and Moon in [km^3 / sec^2]
double GMSun, GMMoon;

// usually using these datetimes ...
using Datetime = dso::datetime<dso::nanoseconds>;

struct EopInfo {
  // actual size of arrays
  int sz;
  // arrays of EOP values extracted from C04
  double mjd[BBSZ], xpa[BBSZ], ypa[BBSZ], ut1a[BBSZ];
};

// to transfer parameters for Variational Equations
struct AuxParams {
  double mjd_tai;
  EopInfo *eopLookUpTables;
  dso::HarmonicCoeffs *hc;
  dso::Mat2D<dso::MatrixStorageType::Trapezoid> *V, *W;
  int degree, order;
  // drag
  dso::Nrlmsise00 *msise;
  dso::nrlmsise00::InParams<
      dso::nrlmsise00::detail::FluxDataFeedType::ST_CSV_SW> *msise_in;
};

int getMeEops(const Datetime &t, char *bulletinb_fn, EopInfo *eopLUT,
              int download = 1) noexcept {
  if (download) {
    // get the corresponding bulletin b file from IERS (download)
    int error = dso::download_iers_bulletinb_for(t.mjd().as_underlying_type(),
                                                 bulletinb_fn);
    if (error) {
      fprintf(stderr, "Failed to download Bulletin B file for date: %ld\n",
              t.mjd().as_underlying_type());
      return 1;
    }
  }

  // parse the Bulletin B file (diregard preliminary values, only use final)
  dso::IersBulletinB_Section1Block bbblocks[35];
  dso::IersBulletinB bulb(bulletinb_fn);
  int bbblocks_size = bulb.parse_section1(bbblocks, false);
  if (bbblocks_size <= 0) {
    fprintf(stderr, "Failed parsing Bulletin B file %s\n", bulletinb_fn);
    return 2;
  }

  for (int i = 0; i < bbblocks_size; i++)
    eopLUT->mjd[i] = static_cast<double>(bbblocks[i].mjd);
  for (int i = 0; i < bbblocks_size; i++)
    eopLUT->xpa[i] = bbblocks[i].x;
  for (int i = 0; i < bbblocks_size; i++)
    eopLUT->ypa[i] = bbblocks[i].y;
  for (int i = 0; i < bbblocks_size; i++)
    eopLUT->ut1a[i] = bbblocks[i].dut1;

  eopLUT->sz = bbblocks_size;

  return 0;
}

// return xp, yp in milliarcsecond [mas]
// and dut1 in millisecond [ms]
int getEop(double mjd_utc, const EopInfo *eops, double &xp, double &yp,
           double &dut1) noexcept {
  // apply corrections; first use INTERP to interpolate x,y,ut1 values for
  // the given date (in [mas], [msec])
  if (iers2010::interp_pole(eops->mjd, eops->xpa, eops->ypa, eops->ut1a,
                            eops->sz, mjd_utc, xp, yp, dut1)) {
    fprintf(stderr, "ERROR. Failed call to interp_pole\n");
    return 1;
  }

  // account for variations in polar motion (Dx,Dy) ocean-tides; results in
  // [μas] and [μsec]
  double dxp, dyp, dut2;
  if (iers2010::ortho_eop(mjd_utc, dxp, dyp, dut2)) {
    fprintf(stderr, "ERROR. Failed call to ortho_eop!\n");
    return 4;
  }

  // transform to [mas] [msec]
  xp += dxp * 1e-3;
  yp += dyp * 1e-3;
  dut1 += dut2 * 1e-3;

  // account for libration effects; results in [μas]
  if (iers2010::pmsdnut2(mjd_utc, dxp, dyp)) {
    fprintf(stderr, "ERROR Failed call to pmsdnut2!\n");
    return 4;
  }

  // transform to [mas]
  xp += dxp * 1e-3;
  yp += dyp * 1e-3;

  return 0;
}

int getEop(const Datetime &t, const EopInfo *eops, double &xp, double &yp,
           double &dut1) noexcept {
  return getEop(t.as_mjd(), eops, xp, yp, dut1);
}

// handle gravity field
int gravity(const char *gfn, int degree, int order, dso::HarmonicCoeffs &hc) {
  dso::Icgem gfc(gfn);

  // parse the header ...
  if (gfc.parse_header()) {
    fprintf(stderr, "ERROR! Failed to parse icgem header!\n");
    return 1;
  }

  assert(degree <= gfc.degree() && order <= degree && hc.degree() == degree);

  // parse data; store coefficients to hc
  if (gfc.parse_data(degree, order, &hc)) {
    fprintf(stderr, "ERROR! Failed to parse harmonic coefficients\n");
    return 1;
  }

  // de-normalize the harmonics coeffs
  hc.denormalize();

  return 0;
}

Eigen::Matrix<double, 3, 3>
ter2cel(double mjd_tai, const EopInfo *eopLUT,
        Eigen::Matrix<double, 3, 3> *dt2c) noexcept {

  // keep small part, do computations with this
  double mjd_days;
  const double taif = std::modf(mjd_tai, &mjd_days);
  const int leap_sec =
      dso::dat(dso::modified_julian_day(static_cast<int>(mjd_tai)));
  const double utcf = taif - static_cast<double>(leap_sec) / 86400e0;

  // we now have date in UTC, get EOP values
  double xp, yp, dut1;
  if (getEop(mjd_days + utcf, eopLUT, xp, yp, dut1)) {
    fprintf(stderr, "ERROR. Failed getting EOP values");
  }

  const double ut1f = utcf + (dut1 / 86400e0) * 1e-3;
  const double ttf = taif + (32184e-3 / 86400e0);

  // const double mjd_utc = mjd_days + utcf;
  const double mjd_ut1 = mjd_days + ut1f;
  const double mjd_tt = mjd_days + ttf;

  // Form the celestial-to-intermediate matrix for this TT.
  auto rc2i = iers2010::sofa::c2i06a(dso::mjd0_jd, mjd_tt);
  
  // Predict the Earth rotation angle for this UT1.
  const double era = iers2010::sofa::era00(dso::mjd0_jd, mjd_ut1);
  
  // Estimate s'.
  const double sp = iers2010::sofa::sp00(dso::mjd0_jd, mjd_tt);
  
  // Form the polar motion matrix.
  auto rpom =
      iers2010::sofa::pom00(xp * iers2010::DMAS2R, yp * iers2010::DMAS2R, sp);
  
  // Combine to form the celestial-to-terrestrial matrix.
  auto mat = rpom * dso::Mat3x3::RotZ(era) * rc2i;
  
  // note that the following will result in an Eigen matrix that is the
  // transpose of mat (Eigen uses Column-Major and Mat3x3 Row-Major)
  Eigen::Matrix<double, 3, 3> t2c(mat.data);

  // ERA derivative
  if (dt2c) {
    const dso::Mat3x3 S({0e0, iers2010::OmegaEarth, 0e0, -iers2010::OmegaEarth,
                         0e0, 0e0, 0e0, 0e0, 0e0});
    mat = rpom * (S * dso::Mat3x3::RotZ(era)) * rc2i;
    *dt2c = Eigen::Matrix<double, 3, 3>(mat.data);
  }

  return t2c;
}

void SunMoon(double mjd_tai, const Eigen::Matrix<double, 3, 1> &rsat,
             Eigen::Matrix<double, 3, 1> &sun_acc,
             Eigen::Matrix<double, 3, 1> &moon_acc,
             Eigen::Matrix<double, 3, 1> &sun_pos,
             Eigen::Matrix<double, 3, 3> &mon_partials) noexcept {

  // TAI to TT (MJD)
  double mjd_days;
  const double taif = std::modf(mjd_tai, &mjd_days);
  const double ttf = taif + (32184e-3 / 86400e0);
  const double mjd_tt = mjd_days + ttf;

  const double jd = mjd_tt + dso::mjd0_jd; // date as JD (TT)
  double rsun[3], rmon[3];

  // position vector of sun/moon, in J2000, [km]
  dso::cspice::j2planet_pos_from(dso::cspice::jd2et(jd), 10, 399, rsun);
  dso::cspice::j2planet_pos_from(dso::cspice::jd2et(jd), 301, 399, rmon);

  Eigen::Matrix<double, 3, 1> rSun(rsun); // [km]
  Eigen::Matrix<double, 3, 1> rMon(rmon); // [km]

  // Sun-induced acceleration [km/sec^2]
  sun_acc = dso::point_mass_accel(GMSun, rsat * 1e-3, rSun);
  sun_acc = sun_acc * 1e-3; // [m/sec^2]

  // Moon-induced acceleration [m/sec^2]
  moon_acc =
      dso::point_mass_accel(GMMoon * 1e9, rsat, rMon * 1e3, mon_partials);

  // Sun position in [m]
  sun_pos = rSun * 1e3;

  return;
}

void setNrlmsise00Params(
    double mjd_ut, const Eigen::Matrix<double, 3, 1> r_ecef, int &newmjd,
    double &secday,
    dso::nrlmsise00::InParams<
        dso::nrlmsise00::detail::FluxDataFeedType::ST_CSV_SW>
        *msise_in) noexcept {

  // time data
  double dmjd;
  secday = std::modf(mjd_ut, &dmjd);
  secday *= 86400e0;
  dso::modified_julian_day mjd((long)dmjd);
  const auto ydoy = mjd.to_ydoy();
  msise_in->params_.year = ydoy.__year.as_underlying_type();
  msise_in->params_.doy = ydoy.__doy.as_underlying_type();
  msise_in->params_.sec = secday;
  newmjd = mjd.as_underlying_type();

  // spatial data -- note, could use a constant earth radius instead of N --
  auto r_ell = dso::car2ell<dso::ellipsoid::grs80>(r_ecef);
  const double glong = dso::rad2deg(r_ell(0)); // [degrees]
  const double glat = dso::rad2deg(r_ell(1));  // [degrees]
  const double alt = r_ell(2)*1e-3; // [km]
  msise_in->params_.glon = glong;
  msise_in->params_.glat = glat;
  msise_in->params_.alt  = alt;

  // STL=SEC/3600+GLONG/15
  msise_in->params_.lst = secday/3600e0 + glong/15e0;

  return;
}

void VariationalEquations(
    double tsec, // TAI
    // state and state transition matrix (inertial RF)
    const Eigen::VectorXd &yPhi,
    // state derivative and state transition matrix derivative (inertial RF)
    Eigen::Ref<Eigen::VectorXd> yPhiP,
    // auxiliary parametrs
    void *pAux) noexcept {

  // void pointer to AuxParameters
  AuxParams *params = static_cast<AuxParams *>(pAux);

  // current mjd, TAI
  const double cmjd = params->mjd_tai + tsec / dso::sec_per_day;

  // terretrial to celestial for epoch
  Eigen::Matrix<double, 3, 3> t2c(
      ter2cel(cmjd, params->eopLookUpTables, nullptr));

  // split position and velocity vectors (inertial)
  Eigen::Matrix<double, 3, 1> r = yPhi.block<3, 1>(0, 0);
  Eigen::Matrix<double, 3, 1> v = yPhi.block<3, 1>(3, 0);

  // compute gravity-induced acceleration
  Eigen::Matrix<double, 3, 3> gpartials;
  Eigen::Matrix<double, 3, 1> r_geo = t2c.transpose() * r;
  Eigen::Matrix<double, 3, 1> gacc = dso::grav_potential_accel(
      r_geo, params->degree, params->order, *(params->V), *(params->W),
      *(params->hc), gpartials);

  // fucking crap! gravity acceleration in earth-fixed frame; need to
  // have inertial acceleration!
  gacc = t2c * gacc;
  gpartials = t2c.transpose() * gpartials * t2c;

  // third body perturbations, Sun and Moon [m/sec^2] in celestial RF
  Eigen::Matrix<double, 3, 1> rsun; // position of sun, [m] in celestial RF
  Eigen::Matrix<double, 3, 1> sun_acc;
  Eigen::Matrix<double, 3, 1> mon_acc;
  Eigen::Matrix<double, 3, 3> tb_partials;
  SunMoon(cmjd, r, sun_acc, mon_acc, rsun,
          tb_partials);
  if (!include_third_body) {
    sun_acc = Eigen::Matrix<double, 3, 1>::Zero();
    mon_acc = Eigen::Matrix<double, 3, 1>::Zero();
    tb_partials = Eigen::Matrix<double, 3, 3>::Zero();
  }

  // Drag
  // set input parameters (time and spatial)
  Eigen::Matrix<double, 3, 1> drag_acc = Eigen::Matrix<double, 3, 1>::Zero();
  if (include_drag) {
    int msise_mjd;
    double msise_secday;
    setNrlmsise00Params(cmjd, r_geo, msise_mjd, msise_secday, params->msise_in);
    //printf("\tparams set for setNrlmsise00Params ...\n");
    // update flux data
    if (params->msise_in->update_params(msise_mjd, msise_secday)) {
      fprintf(stderr, "ERROR. Failed to update flux/Ap data from drag!\n");
    }
    //printf("\tparams updated in hunter ...\n");
    // get density (kg/m^3)
    double density;
    {
      dso::nrlmsise00::OutParams out;
      //params->msise_in->params_.dump_params();
      params->msise->gtd7d(&params->msise_in->params_, &out);
      density = out.d[5];
      //printf("\tdensity value is: %+.15e\n", out.d[5]);
    }
    // we also need to true-to-date matrix (for relavite velocity)
    Eigen::Matrix<double, 3, 3> r_tof;
    {
      double mjd_days;
      const double taif = std::modf(cmjd, &mjd_days);
      const double ttf = taif + (32184e-3 / 86400e0);
      const double mjd_tt = mjd_days + ttf;
      const auto rnpb = iers2010::sofa::pnm06a(dso::mjd0_jd, mjd_tt);
      Eigen::Matrix<double, 3, 3> r_toft(rnpb.data);
      r_tof = r_toft.transpose();
    }
    const auto drag =
        dso::drag_accel(r, v, r_tof, Area_H2C, CD, Mass_H2C, density);
    drag_acc = drag;
    //printf("\tdrag computed, done!\n");
  }

  // SRP
  Eigen::Matrix<double, 3, 1> srp = Eigen::Matrix<double, 3, 1>::Zero();
  if (include_srp) {
    dso::Vector3 rV({r(0), r(1), r(2)});
    dso::Vector3 sV({rsun(0), rsun(1), rsun(2)});
    if (utest::montebruck_shadow(rV, sV)) {
      srp = dso::solar_radiation_acceleration(r, rsun, Area_H2C, Mass_H2C,
                                              Cr_H2C);
    }
  }

  // State transition (skip first column which is the state vector)
  Eigen::Matrix<double, 6, 6> Phi(yPhi.data() + 6);

  // derivative of state transition matrix, aka
  // | v (3x3)   0 (3x3)     I (3x3)   |
  // | a (3x1) da/dr (3x3) da/dv (3x3) |
  // because dv/dr = 0 and
  //         da/dr = I
  Eigen::Matrix<double, 6, 6> dfdy;
  dfdy.block<3, 3>(0, 0) = Eigen::Matrix<double, 3, 3>::Zero();
  dfdy.block<3, 3>(0, 3) = Eigen::Matrix<double, 3, 3>::Identity();
  dfdy.block<3, 3>(3, 0) = gpartials + tb_partials;
  dfdy.block<3, 3>(3, 3) = Eigen::Matrix<double, 3, 3>::Zero();

  // Derivative of combined state vector and state transition matrix
  // dPhi = dfdy * Phi;
  Eigen::Matrix<double, 6, 7> yPhip;
  yPhip.block<6, 6>(0, 1) = dfdy * Phi;

  // state derivative (aka [v,a]), in one (first) column
  yPhip.block<3, 1>(0, 0) = v;
  yPhip.block<3, 1>(3, 0) = gacc + sun_acc + mon_acc + srp + drag_acc;

  // matrix to vector (column-wise)
  yPhiP = Eigen::VectorXd(
      Eigen::Map<Eigen::VectorXd>(yPhip.data(), yPhip.cols() * yPhip.rows()));
  
  //printf("Accelerations: \n");
  //printf("Gravity     Sun          Moon         srp          drag\n");
  //for (int i=0; i<3; i++) {
  //  printf("%+12.9f %+12.9f %+12.9f %+12.9f %+12.9f\n", gacc(i), sun_acc(i), mon_acc(i), srp(i), drag_acc(i));
  //}

  ++call_nr;
  return;
}

YAML::Node get_yaml_node(const YAML::Node *root, const char *key) {
  try {
  return root->operator[](std::string(key));
  } catch (...) {
    fprintf(stderr, "Failed finding node %s\n", key);
    std::exit(99);
  }
}

const char *get_yaml_value_depth2(const YAML::Node *root, const char *key1,
                                  const char *key2, char *buf) {
  const YAML::Node node = get_yaml_node(root, key1);
  try {
  std::string result = node[std::string(key2)].as<std::string>();
  std::strcpy(buf, result.c_str());
  } catch (...) {
    fprintf(stderr, "Failed finding key %s::%s\n", key1, key2);
    std::exit(98);
  }
  return buf;
}

const char *get_yaml_value_depth3(const YAML::Node *root, const char *key1,
                                  const char *key2, const char *key3, 
                                  char *buf) {
  const YAML::Node node = get_yaml_node(root, key1);
  return get_yaml_value_depth2(&node, key2, key3, buf);
}


int main(int argc, char *argv[]) {

  if (argc != 2 && argc!=3) {
    fprintf(stderr, "Usage %s <YAML CONFIG> [INTEGRATION INTERVAL in sec]\n", argv[0]);
    return 1;
  }
  const YAML::Node config = YAML::LoadFile(argv[1]);
  char buf[256];
  
  // to compute the planet's position via cspice, we need to load:
  // 1. the planetary ephemeris (SPK) kernel
  // 2. the leap-second (aka LSK) kernel
  // 3. the planetary constants kernel (PCK)
  dso::cspice::load_if_unloaded_spk(
      get_yaml_value_depth2(&config, "naif-kernels", "spk", buf));
  dso::cspice::load_if_unloaded_lsk(
      get_yaml_value_depth2(&config, "naif-kernels", "lsk", buf));
  // well, actually we do not need to load the kernel; just get the values we
  // want!
  // dso::cspice::load_if_unloaded_pck(argv[7]);
  get_yaml_value_depth2(&config, "naif-kernels", "pck", buf);
  dso::getSunMoonGM(buf, GMSun, GMMoon); // [km^3 / sec^2]

  // parse degree and order of gravity field
  int degree = -1, order = -1;
  get_yaml_value_depth2(&config, "gravity", "degree", buf);
  auto res = std::from_chars(buf, buf+255, degree);
  get_yaml_value_depth2(&config, "gravity", "order", buf);
  res = std::from_chars(buf, buf + 255, order);
  if (degree < 0 || order < 0 || order > degree) {
    fprintf(
        stderr,
        "ERROR. Failed to parse or erronuous values for degree/order: %d/%d\n",
        degree, order);
    return 1;
  }

  // Harmonic coefficients
  printf("* setting up harmonic coefficients ...\n");
  dso::HarmonicCoeffs hc(degree);
  if (gravity(get_yaml_value_depth2(&config, "gravity", "model", buf), degree, order, hc)) {
    fprintf(stderr,
            "ERROR. Failed to compute harmonic coefficients. Aborting ...\n");
    return 1;
  }

  // Lagrange polynomials (depend on position) N+1 (for zero offset) and +2
  // because we are computing potential partials. Allocate space ...
  dso::Mat2D<dso::MatrixStorageType::Trapezoid> V(degree + 3, order + 3),
      W(degree + 3, order + 3);

  // Handle the Sp3 file, prepare for parsing ...
  printf("* handling input sp3 file ...\n");
  dso::Sp3c sp3(get_yaml_value_depth2(&config, "data", "sp3", buf));

  // a satellite instance, will hold the sp3 satellite
  SatelliteId sv;
  dso::Sp3DataBlock block;

  if (sp3.num_sats() == 1) {
    sv.set_id(sp3.sattellite_vector()[0].id);
  } else {
    fprintf(stderr, "More than one Satellites in sp3 file. This test program "
                    "is only meant to work with one.\n");
    return 1;
  }

  // Handle EOPs:
  // Download bulletin B for the date, parse file and store Look Up Tables
  // in an EopInfo instance (for xp, yp, dut1)
  printf("* downloading and parsing Bulletin B file ...\n");
  const auto t0 = sp3.start_epoch();
  char bulletinb_fn[256];
  EopInfo EopLookUpTables;
  if (getMeEops(t0, bulletinb_fn, &EopLookUpTables)) {
    fprintf(stderr, "Error. Failed to fetch EOPs data\n");
    return 3;
  }

  // Atmospheric Drag
  // 1. Create an NRLMSISE00 instance to be called within the VE
  dso::Nrlmsise00 Msise;
  // 2. Create an instance for NRLMSISE00 data feed; create it once so it
  //    handles all IO and fetching keeping dates, so that we don't have to
  //    do it for each call
  get_yaml_value_depth3(&config, "force-model", "atmospheric-drag", "atmo-data-csv", buf);
  dso::nrlmsise00::InParams<
      dso::nrlmsise00::detail::FluxDataFeedType::ST_CSV_SW>
      MsiseParams(buf, sp3.start_epoch().mjd(), 0e0);
  // set
  // 1. all switches on (default)
  // 2. output in meters
  // 3. use hourly ap
  MsiseParams.params_.set_switches_on();
  MsiseParams.params_.meters_on();
  MsiseParams.params_.use_aparray();

  // Create Auxiliary parameters. This will be passed-in the variational
  // equations to compute derivs.
  AuxParams params{sp3.start_epoch().as_mjd(),
                   &EopLookUpTables,
                   &hc,
                   &V,
                   &W,
                   degree,
                   order,
                   &Msise,
                   &MsiseParams};

  // SetUp an integrator
  printf("* setting up integrator ...\n");
  const double relerr = 1e-12;  // Relative and absolute
  const double abserr = 1e-12; // accuracy requirement
  dso::SGOde Integrator(VariationalEquations, 6 + 6*6, relerr, abserr, &params);

  // matrices ...
  Eigen::Matrix<double, 6+6*6, 1> yPhi;     // state + var. equations
  Eigen::VectorXd sol(6+6*6);               // integrator solution
  Eigen::Matrix<double, 6*6,1> I6x6_vec;
  Eigen::VectorXd r0_geo(6), r0_cel(6);

  // Time
  // (remember) const auto t0 = sp3.start_epoch();
  auto epoch = t0;                  // current
  auto step = sp3.interval(); // integration interval
  if (argc == 3) {
      step = dso::nanoseconds(std::atoi(argv[2]) * 1'000'000'000L);
  }

  // let's try reading the records; note that -1 denotes EOF
  int error = 0;
  std::size_t rec_count = 0;
  printf("* iterating ...\n");
  do {

    // read next record ...
    if ((error = sp3.get_next_data_block(sv, block)) > 0) {
      fprintf(stderr, "Something went wrong ....status = %3d\n", error);
      return 1;
    }

    // check the heath status
    bool position_ok = !block.flag.is_set(dso::Sp3Event::bad_abscent_position);

    if (position_ok) {

      // accumulate state (m, m/sec), earth-fixed
      r0_geo << block.state[0] * 1e3, block.state[1] * 1e3,
          block.state[2] * 1e3, block.state[4] * 1e-1, block.state[5] * 1e-1,
          block.state[6] * 1e-1;

      // Terrestrial to Celestial transformation matrix and derivative
      Eigen::Matrix<double, 3, 3> dt2c;
      Eigen::Matrix<double, 3, 3> t2c(
          ter2cel(block.t.as_mjd(), params.eopLookUpTables, &dt2c));

      // transform geocentric state to inertial
      r0_cel.block<3, 1>(0, 0) = t2c * r0_geo.block<3, 1>(0, 0);
      r0_cel.block<3, 1>(3, 0) =
          t2c * r0_geo.block<3, 1>(3, 0) + dt2c * r0_geo.block<3, 1>(0, 0);

      if (Integrate) {

        // Vector containing state + variational equations size: 6 + 6x6
        // Ref. Frame: inertial
        yPhi = Eigen::Matrix<double, 6 + 6*6, 1>::Zero();
        yPhi.block<6,1>(0,0) = r0_cel;
        {
          int k = 6;
          for (int col = 1; col < 7; col++)
            for (int row = 0; row < 6; row++)
              yPhi(k++) = (col - 1 == row) ? 1e0 : 0e0;
        }

        // t0 for variational equations (TAI)
        params.mjd_tai = block.t.as_mjd();

        // target t for variational equations; seconds after t0
        double tout = step.to_fractional_seconds();

        // seconds after t0 for integrator
        double t = 0e0;

        // integrate (in inertial RF), from 0 to step
        // the "real" date (in mjd) inside the integrator is:
        // params.mjd_tai + t / 86400
        Integrator.flag() = 1;
        Integrator.de(t, tout, yPhi, sol);
        if (std::abs(tout - t) > 5) {
          fprintf(stderr,
                  "Warning! Interpolation ended more than 5 secs away target: "
                  "%.6f reached: %.6f\n",
                  tout, t);
        }
        // solution vector stored in sol
        // time is t seconds after initial t (which is 0), hence time
        // reached is: block.t(=params.mjd_tai) + t/86400

        // output epoch as datetime
        epoch = block.t;
        epoch.add_seconds(
            dso::nanoseconds(static_cast<unsigned long>(t * 1e9)));
        dso::strftime_ymd_hmfs(epoch, buf);

        // transform solution to terrestrial for SP3 comparisson
        t2c = ter2cel(epoch.as_mjd(), params.eopLookUpTables, &dt2c);
        r0_geo.block<3, 1>(0, 0) = t2c.transpose() * sol.block<3, 1>(0, 0);
        r0_geo.block<3, 1>(3, 0) = t2c.transpose() * sol.block<3, 1>(3, 0) +
                                   dt2c.transpose() * sol.block<3, 1>(0, 0);

        // print results in terestrial coordinates
        printf("%s %+15.4f %+15.4f %+15.4f %+15.7f %+15.7f %+15.7f %18.9f\n",
               buf, r0_geo(0), r0_geo(1), r0_geo(2), r0_geo(3), r0_geo(4),
               r0_geo(5), epoch.as_mjd());

        // print results in celestial coordinates
        // printf("%s %+15.4f %+15.4f %+15.4f %+15.7f %+15.7f %+15.7f %18.9f\n",
        //       buf, sol(0), sol(1), sol(2), sol(3), sol(4),
        //       sol(5), epoch.as_mjd());

      } else {

        // Do not integrate, just report results for this epoch
        // Ref. Frame: Earth-fixed (as in sp3 file)
        // Time Scale: the one recorded in the sp3 file, probably TAI
        // Units     : [m] and [m/sec]

        epoch = block.t;
        dso::strftime_ymd_hmfs(epoch, buf);

        // print terrestrial satellite oordinates
        printf("%s %+15.4f %+15.4f %+15.4f %+15.7f %+15.7f %+15.7f %18.9f\n",
               buf, r0_geo(0), r0_geo(1), r0_geo(2), r0_geo(3), r0_geo(4),
               r0_geo(5), epoch.as_mjd());

        // print celestial satellite oordinates
        // printf("%s %+15.4f %+15.4f %+15.4f %+15.7f %+15.7f %+15.7f %18.9f\n",
        //       buf, r0_cel(0), r0_cel(1), r0_cel(2), r0_cel(3), r0_cel(4),
        //       r0_cel(5), epoch.as_mjd());
      }

      ++rec_count;
    }

    if (epoch.delta_sec(t0).to_fractional_seconds() > 12 * 3600e0)
      break;

  } while (!error && rec_count < 1000);

  printf("Num of records read: %6lu\n", rec_count);
  return (error == -1);
}
