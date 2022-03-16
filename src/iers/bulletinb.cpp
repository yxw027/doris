#include "cweb.hpp"
#include "iers_bulletin.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

constexpr const std::size_t MAX_LINE_CHARS = 512;

// retrun a pointer to the first non-whitespace char
inline char *nws(char *line) noexcept {
  char *c = line;
  while (*c && *c == ' ')
    ++c;
  return c;
}

dso::IersBulletinB::IersBulletinB(const char *fn) {
  assert(std::strlen(fn) < 256);
  std::strcpy(filename, fn);
  stream.open(filename);
  if (!stream.is_open()) {
    fprintf(stderr,
            "[ERROR] Failed opening Bulltin B file %s (traceback: %s)\n",
            filename, __func__);
    throw std::runtime_error("Failed opening file");
  }
}

dso::IersBulletinB::IersBulletinB(dso::IersBulletinB &&other) noexcept {
  std::strcpy(filename, other.filename);
  stream.swap(other.stream);
}

dso::IersBulletinB &
dso::IersBulletinB::operator=(dso::IersBulletinB &&other) noexcept {
  std::strcpy(filename, other.filename);
  stream.swap(other.stream);
  return *this;
}

dso::IersBulletinB::~IersBulletinB() noexcept {
  filename[0] = '\0';
  if (stream.is_open())
    stream.close();
}

int reach_section1(std::ifstream &fin, bool &has_dUt1UtcR) noexcept {
  char line[MAX_LINE_CHARS], *c, *end;
  fin.seekg(0);

  // first line should be a whitespace chars, and 'BULLETIN B XXX'
  fin.getline(line, MAX_LINE_CHARS);
  c = nws(line);
  if (std::strncmp(c, "BULLETIN B ", 11))
    return 1;
  int bulnum = std::strtol(c + 11, &end, 10);
  if (!bulnum || (c == end))
    return 1;

  // ignore next couple of lines ...
  // 1. next line is date [ignored]
  // 2. next is empty [ignored]
  // 3. next is 'Contents are described in ...' [ignored]
  // 4. next is empty [ignored]
  // for (int i = 0; i < 4; i++)
  //   fin.getline(line, MAX_LINE_CHARS);
  // !!BUT!! seems that the above might change and more empty lines may be
  // injected. Hence, just skip all lines untill we reach
  // '1 - DAILY FINAL VALUES OF x, y, UT1-UTC, dX, dY'
  int dummy_it = 0, max_dummy_it = 10;
  fin.getline(line, MAX_LINE_CHARS);
  while ((std::strncmp(nws(line),
              "1 - DAILY FINAL VALUES OF x, y, UT1-UTC, dX, dY", 48) &&
          std::strncmp(
              nws(line),
              "1 - DAILY FINAL VALUES OF  x, y, UT1-UTC, dX, dY", 49) &&
          std::strncmp(
              nws(line),
              "1 - DAILY SMOOTHED VALUES OF  x, y, UT1-UTC, UT1R-UTC, dX, dY",
              62)) &&
         (dummy_it++ < max_dummy_it)) {
    fin.getline(line, MAX_LINE_CHARS);
  }
  if (dummy_it >= max_dummy_it)
    return 500;

  // 1 - DAILY FINAL VALUES OF x, y, UT1-UTC, dX, dY (already read, see above)
  // Next is:
  // 'Angular unit is milliarcsecond (mas), time unit is millisecond (ms).'
  fin.getline(line, MAX_LINE_CHARS);
  c = nws(line);
  if (std::strncmp(c,
                   "Angular unit is milliarcsecond (mas), time unit is "
                   "millisecond ",
                   63))
    return 7;

  // 'Upgraded solution from March 1 2017 - consistent with ITRF 2014.'
  // well, actually this is non-standard, could be  e.g.
  // 'The reference systems are described in the 2006 IERS Annual Report.'
  fin.getline(line, MAX_LINE_CHARS);
  // an empty line follows; ignore
  fin.getline(line, MAX_LINE_CHARS);

  // next two lines describe records
  fin.getline(line, MAX_LINE_CHARS);
  c = nws(line);
  if (!std::strncmp(
          c,
          "DATE     MJD       x       y      UT1-UTC      dX     dY   "
          "  x err    y err   UT1 err  dX err  dY err",
          102)) {
    has_dUt1UtcR = false;
  } else if (!std::strncmp(
          c,
          "DATE     MJD       x       y      UT1-UTC      dX     dY   "
          "  x err    y err   UT1 err  X err  Y err",
          100)) {
    has_dUt1UtcR = false;
  } else if (!std::strncmp(
                 c,
                 "DATE     MJD       x       y      UT1-UTC  UT1R-UTC    dX    "
                 " dY     x err    y err   UT1 err  X err  Y err",
                 108)) {
    has_dUt1UtcR = true;
  } else {
    #ifdef DEBUG
    fprintf(stderr, "ERROR. Failed matching line [%s] (traceback: %s)\n", c, __func__);
    #endif
    return 10;
  }

  fin.getline(line, MAX_LINE_CHARS);
  c = nws(line);
  if ((!has_dUt1UtcR) &&
      std::strncmp(c,
                   "(0 h UTC)            mas     mas       ms         mas    "
                   "mas     mas      mas      ms     mas     mas",
                   102))
    return 11;
  if (has_dUt1UtcR &&
      std::strncmp(c,
                   "(0 h UTC)            mas     mas       ms        ms       "
                   "mas    mas     mas      mas      ms     mas     mas",
                   110))
    return 11;

  // an empty line follows; ignore
  fin.getline(line, MAX_LINE_CHARS);

  // next two line should be 'Final values'
  fin.getline(line, MAX_LINE_CHARS);
  c = nws(line);
  if (std::strncmp(c, "Final values", 12))
    return 13;

  // skip next three lines ...
  for (int i = 0; i < 3; i++)
    fin.getline(line, MAX_LINE_CHARS);
  return !fin.good();
}

int parse_section1_line_mjd(const char *line, long &mjd) noexcept {
  char *end;
  const char *c = line + 15;
  mjd = std::strtol(c, &end, 10);
  return (!mjd || c == end);
}

int parse_section1_line(const char *line, bool has_dUt1UtcR,
                        dso::IersBulletinB_Section1Block &block) noexcept {
  char *end;
  const char *c = line + 15;
  block.mjd = std::strtol(c, &end, 10);
  if (!block.mjd || c == end)
    return 1;

  double tmp[11];
  for (int i = 0; i < 10 + has_dUt1UtcR; i++) {
    c = end + 1;
    tmp[i] = std::strtod(c, &end);
    if (c == end)
      return i;
  }

  block.x = tmp[0];
  block.y = tmp[1];
  block.dut1 = tmp[2];
  block.dUt1rUt =
      (has_dUt1UtcR) ? tmp[3] : dso::bulletin_details::BULLETIN_MISSING_VALUE;
  // here be dUt1Utc_R
  int idx = 3 + has_dUt1UtcR;
  block.dX = tmp[idx++];
  block.dY = tmp[idx++];
  block.xerr = tmp[idx++];
  block.yerr = tmp[idx++];
  block.dut1err = tmp[idx++];
  block.dXerr = tmp[idx++];
  block.dYerr = tmp[idx++];

  return 0;
}

int dso::IersBulletinB::parse_section1(dso::IersBulletinB_Section1Block *block,
                                       bool include_preliminary) noexcept {
  if (int err_line = reach_section1(stream, this->has_dUt1UtcR)) {
    fprintf(stderr,
            "[ERROR] Failed parsing Bulletin B file %s; line nr %d (traceback: "
            "%s)\n",
            filename, err_line, __func__);
    return -1;
  }

  char line[MAX_LINE_CHARS];
  int block_nr = 0;
  // next line to read from stream is a Section1 line (final)
  stream.getline(line, MAX_LINE_CHARS);
  while (*line && line[0] != ' ') {
    if (parse_section1_line(line, this->has_dUt1UtcR, block[block_nr])) {
      fprintf(stderr,
              "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
              "(traceback: %s)\n",
              filename, __func__);
      return -2;
    }
    block[block_nr++].type = 'F';
    stream.getline(line, MAX_LINE_CHARS);
  }

  // exit if we do not need the preliminary values
  if (!include_preliminary)
    return block_nr;

  // we should now have read all final values, and reached an empty line
  stream.getline(line, MAX_LINE_CHARS);
  if (std::strncmp(line, " Preliminary extension", 22)) {
    fprintf(stderr,
            "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
            "(traceback: %s)\n",
            filename, __func__);
    return -3;
  }

  // next line to read from stream is a Section1 line (preliminery)
  stream.getline(line, MAX_LINE_CHARS);
  while (*line && line[0] != ' ') {
    if (parse_section1_line(line, this->has_dUt1UtcR, block[block_nr])) {
      fprintf(stderr,
              "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
              "(traceback: %s)\n",
              filename, __func__);
      return -4;
    }
    block[block_nr++].type = 'P';
    stream.getline(line, MAX_LINE_CHARS);
  }

  return block_nr;
}

int dso::IersBulletinB::get_section1_at(int imjd,
                                        dso::IersBulletinB_Section1Block &block,
                                        bool include_preliminary) noexcept {
  if (int err_line = reach_section1(stream, this->has_dUt1UtcR)) {
    fprintf(stderr,
            "[ERROR] Failed parsing Bulletin B file %s; line nr %d (traceback: "
            "%s)\n",
            filename, err_line, __func__);
    return 1;
  }

  // int imjd = static_cast<int>(std::floor(t.as_mjd()));
  char line[MAX_LINE_CHARS];
  // next line to read from stream is a Section1 line (final)
  stream.getline(line, MAX_LINE_CHARS);
  // quick check to inspect the file's time interval ...
  long first_mjd;
  if (parse_section1_line_mjd(line, first_mjd)) {
    fprintf(stderr,
            "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
            "(traceback: %s)\n",
            filename, __func__);
    return 2;
  } else if (first_mjd > imjd) {
    return dso::bulletin_details::FILE_IS_AHEAD;
  } else if (imjd > first_mjd + 80) {
    return dso::bulletin_details::FILE_IS_PRIOR;
  }
  while (*line && line[0] != ' ') {
    if (parse_section1_line(line, this->has_dUt1UtcR, block)) {
      fprintf(stderr,
              "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
              "(traceback: %s)\n",
              filename, __func__);
      return 2;
    }
    block.type = 'F';
    if (block.mjd == imjd)
      return 0;
    stream.getline(line, MAX_LINE_CHARS);
  }

  // exit if we do not need the preliminary values
  if (!include_preliminary)
    return -1;

  // we should now have read all final values, and reached an empty line
  stream.getline(line, MAX_LINE_CHARS);
  if (std::strncmp(line, " Preliminary extension", 22)) {
    fprintf(stderr,
            "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
            "(traceback: %s)\n",
            filename, __func__);
    return 3;
  }

  // next line to read from stream is a Section1 line (preliminery)
  stream.getline(line, MAX_LINE_CHARS);
  while (*line && line[0] != ' ') {
    if (parse_section1_line(line, this->has_dUt1UtcR, block)) {
      fprintf(stderr,
              "[ERROR] Failed parsing Bulletin B file %s at Section 1 "
              "(traceback: %s)\n",
              filename, __func__);
      return 4;
    }
    block.type = 'P';
    if (block.mjd == imjd)
      return 0;
    stream.getline(line, MAX_LINE_CHARS);
  }

  return -1;
}

int bulletinb_download_and_check(long mjd, const char *remote,
                                 /*const char *local*/ const fs::path &local, bool &deletefn) noexcept {
  int status;
  if ((status=dso::http_get(remote, local.c_str()))>0) {
    fprintf(
        stderr,
        "ERROR. Failed to download IERS Bulletin B file %s (traceback: %s)\n",
        remote, __func__);
    return 2;
  }

  deletefn = !status;

  // let's see if we guessed right and the date is within the file downloaded
  // initialize a Bulletin B instance
  dso::IersBulletinB bb(local.c_str());

  // section 1 block, to hold file records
  dso::IersBulletinB_Section1Block block;

  return bb.get_section1_at(mjd, block);
}

int dso::download_iers_bulletinb_for(long mjd, const char *dir) noexcept {
  // construct the directory to download files to
  fs::path local = (dir) ? dir : fs::current_path();
  if (!fs::is_directory(local)) {
    fprintf(stderr, "ERROR. %s is not a valid directory (traceback: %s)\n", local.c_str(), __func__);
    return -1;
  }

  char remote[64], fnbuf[64];
  int bnumber = 253;
  long bmjd = 54831;
  if (mjd < bmjd) {
    fprintf(
        stderr,
        "ERROR. Failed to download IERS Bulletin B file for MJD %ld; given "
        "date is prior to first Bulletin file for MJD %ld (traceback: %s)\n",
        mjd, bmjd, __func__);
    return 1;
  }

  // let's make a wild guess ... each bulletin B has values for 31 days;
  int guess = (mjd - bmjd) / 31 + bnumber;
  int initial_guess = guess;

  // auxiliary fs
  bool deletetmp=false;
  std::error_code ec;
  
  // make the filename download and check the file
  std::sprintf(remote, "https://hpiers.obspm.fr/iers/bul/bulb_new/bulletinb.%d",
               guess);
  std::sprintf(fnbuf, "bulletinb.%d", guess);
  local /= fs::path(fnbuf);
  int status = bulletinb_download_and_check(mjd, remote, local, deletetmp);

  // error parsing file
  if (status > 0) {
    fprintf(stderr,
            "ERROR. Failed to parse IERS Bulletin B file %s (traceback: %s)\n",
            local.c_str(), __func__);
    return status;
  }

  // do we have the correct file ?
  if (!status)
    return 0;

  int max_tries = 3;

  // if the file is ahead of the given date ...
  if (status == dso::bulletin_details::FILE_IS_AHEAD) {
    while ((status == dso::bulletin_details::FILE_IS_AHEAD) &&
           (initial_guess - guess < max_tries)) {
      if (deletetmp) fs::remove(local, ec);
      --guess;
      std::sprintf(remote,
                   "https://hpiers.obspm.fr/iers/bul/bulb_new/bulletinb.%d",
                   guess);
      std::sprintf(fnbuf, "bulletinb.%d", guess);
      local.replace_filename(fs::path(fnbuf));
      status = bulletinb_download_and_check(mjd, remote, local,deletetmp);
    }
    // if it is prior ...
  } else if (status == dso::bulletin_details::FILE_IS_PRIOR) {
    while ((status == dso::bulletin_details::FILE_IS_PRIOR) &&
           (guess - initial_guess < max_tries)) {
      if (deletetmp) fs::remove(local, ec);
      ++guess;
      std::sprintf(remote,
                   "https://hpiers.obspm.fr/iers/bul/bulb_new/bulletinb.%d",
                   guess);
      std::sprintf(fnbuf, "bulletinb.%d", guess);
      local.replace_filename(fs::path(fnbuf));
      status = bulletinb_download_and_check(mjd, remote, local,deletetmp);
    }
  }

  #ifdef DEBUG
  printf("%s Number of guesses for the Bulletin B file %d (started with number %d, ended at %d)\n", __func__, std::abs(guess-initial_guess), initial_guess, guess);
  #endif

  // ... so, de we have the right file?
  if (!status)
    return status;

  if (deletetmp) fs::remove(local, ec);
  fprintf(stderr,
          "ERROR. Failed to download any matching IERS Bulletin B file for MJD "
          "%ld (traceback: %s)\n",
          mjd, __func__);
  return status;
}