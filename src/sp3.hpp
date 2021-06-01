#ifndef __SP3C_IGS_FILE__
#define __SP3C_IGS_FILE__

#include "ggdatetime/dtcalendar.hpp"
#include <fstream>
#include <vector>
#include <cstdint>
#ifdef DEBUG
#include "ggdatetime/datetime_write.hpp"
#endif

namespace ids {

/// @enum Sp3Event Describe an event that can be recorded in an Sp3 file
///       A record may be marked with multiple (or none) Sp3Event's
enum class Sp3Event : unsigned int {
  /// Bad or absent positional values are to be set to 0.000000
  bad_abscent_position = 0,
  /// Bad or absent clock values areto be set to _999999.999999.  The six 
  /// integer nines are required, whereasthe fractional part nines are 
  /// optional.
  bad_abscent_clock,
  /// Column 75 is the Clock Event Flag (either 'E' or blank).  An 'E' flag is 
  /// used to denote a discontinuity in the satellite clock correction (this 
  /// might be caused by a clock swap on the satellite). The discontinuity is 
  /// understood to have occurred sometime between the previous epoch and 
  /// current epoch, or at the current epoch. A blank means either no event 
  /// occurred, or it is unknown whether any event occurred.
  clock_event,
  /// Column 76 is theClock Correction Prediction Flag (either 'P' or blank).  
  /// A 'P' flagindicates that the satellite clock correction at this epoch is 
  /// predicted.A blank means that the clock correction is observed.
  clock_prediction,
  /// Column 79 is theorbit Maneuver Flag (either 'M' or blank).  An 'M' flag 
  /// indicates thatsometime between the previous epoch and the current epoch, 
  /// or at the currentepoch, an orbit maneuver took place for this satellite.  
  /// As an example, if a certain maneuver lasted 50 minutes (a satellite 
  /// changing orbital planes)then these M-flags could conceivably appear at 
  /// five separate 15-minute orbitepochs. If the maneuver started at 11h 14m 
  /// and lasted to 12h 04m, M-flagswould appear for the epochs 11:15, 11:30, 
  /// 11:45, 12:00 and 12:15.  Amaneuver is loosely defined as any planned or 
  /// humanly-detectable thrusterfiring that changes the orbit of a satellite.  
  /// A blank means either nomaneuver occurred, or it is unknown whether any 
  /// maneuver occurred.
  maneuver,
  /// Column 80 is the Orbit Prediction Flag (either 'P' or blank).  A 'P' 
  /// flag indicates that the satellite position at this epoch is predicted. A 
  /// blank means thatthe satellite position is observed.
  orbit_prediction,
  /// Reocord has valid position std. deviation records
  has_pos_stddev,
  /// Reocord has valid clock std. deviation records
  has_clk_stddev,
  /// Bad or absent velocity (positional) values are to be set to 0.000000
  bad_abscent_velocity,
  /// Bad or absent clock rate values
  bad_abscent_clock_rate
};// Sp3Event

static_assert(std::numeric_limits<unsigned char>::digits >
              static_cast<unsigned int>(Sp3Event::orbit_prediction));

/// @class Sp3Flag A flag to hold all events recorded in the Sp3 for a field
struct Sp3Flag {
  /// Initialize unmarked
  unsigned char bits_{0};
  /// Mark flag with an Sp3Event (aka, set the Sp3Event)
  void set(Sp3Event e) noexcept {
    bits_ |= (1 << static_cast<unsigned char>(e));
  }
  /// Un-Mark flag with an Sp3Event (aka, unset the Sp3Event)
  void clear(Sp3Event e) noexcept {
    bits_ &= (~(1 << static_cast<unsigned char>(e)));
  }
  /// Clear all Sp3Event's and reset flag to empty/clean
  void reset() noexcept { bits_ = 0; }
  /// Trigger, aka check if an Sp3Event is set
  bool is_set(Sp3Event e) const noexcept {
    return ((bits_ >> static_cast<unsigned char>(e)) & 1);
  }
  /// Check if Sp3Flag is clean (no Sp3Event is set)
  bool is_clean() const noexcept {
    return !bits_;
  }
};// Sp3Flag

/// @class Satellite ID as denoted in an Sp3 file
struct SatelliteId { 
  char id[3]={'\0'};
  explicit SatelliteId(const char* str) noexcept {
    std::memcpy(id, str, 3);
  }
};

class Sp3c {
public:
  /// Let's not write this more than once.
  typedef std::ifstream::pos_type pos_type;

  /// @brief Constructor from filename
  explicit Sp3c(const char *);

  /// @brief Destructor (closing the file is not mandatory, but nevertheless)
  ~Sp3c() noexcept {
    if (__istream.is_open())
      __istream.close();
  }

  /// @brief Copy not allowed !
  Sp3c(const Sp3c &) = delete;

  /// @brief Assignment not allowed !
  Sp3c &operator=(const Sp3c &) = delete;

  /// @brief Move Constructor.
  Sp3c(Sp3c &&a) noexcept(
      std::is_nothrow_move_constructible<std::ifstream>::value) = default;

  /// @brief Move assignment operator.
  Sp3c &operator=(Sp3c &&a) noexcept(
      std::is_nothrow_move_assignable<std::ifstream>::value) = default;

  auto interval() const noexcept { return interval__; }

  auto num_sats() const noexcept { return num_sats__; }

  int get_next_data_block() noexcept;

#ifdef DEBUG
void print_members() const noexcept;
#endif

private:
  /// @brief Read sp3c header; assign info
  int read_header() noexcept;

  /// @brief Get and resolve the next Position and Clock Record
  int get_next_position(SatelliteId& sat, double& xkm, double& ykm, double& zkm, double& clk, 
double& xstdv, double& ystdv, double& zstdv, double& cstdv, Sp3Flag& flag) noexcept;

  std::string __filename;  ///< The name of the file
  std::ifstream __istream; ///< The infput (file) stream
  char version__;          ///< the version 'c' or 'd'
  ngpt::datetime<ngpt::microseconds> start_epoch__; ///< Start epoch
  int num_epochs__,              ///< Number of epochs in file
      num_sats__;                ///< Number od SVs in file
  char crd_sys__[6] = {'\0'},///< Coordinate system (last char always '\0')
    orb_type__[4] = {'\0'}, ///< Orbit type (last char always '\0')
    agency__[5] = {'\0'},  ///< Agency (last char always '\0')
    time_sys__[4] = {'\0'}; ///< Time system (last char always '\0')
  ngpt::microseconds interval__; ///< Epoch interval
  //SATELLITE_SYSTEM __satsys;     ///< satellite system
  pos_type __end_of_head;        ///< Mark the 'END OF HEADER' field
  std::vector<SatelliteId> sat_vec__; ///< Vector of satellite id's
  double fpb_pos__, ///< floating point base for position std. dev (mm or 10**-4 mm/sec)
    fpb_clk__; ///< floating point base for clock std. dev (psec or 10**-4 psec/sec)
};// Sp3c

}// ids

#endif
