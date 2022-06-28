#include "sgode.hpp"
#include <limits>

constexpr const double umach = std::numeric_limits<double>::epsilon();
constexpr const double twou   = 2e0*umach;
constexpr const double fouru  = 4e0*umach;
constexpr const double two[] = {2e0,4e0,8e0,16e0,32e0,64e0,128e0,256e0,
     512e0,1024e0,2048e0,4096e0,8192e0};
constexpr const double gstr[] = {5e-1,0.0833e0,0.0417e0,0.0264e0,0.0188e0,0.0143e0,
     0.0114e0,0.00936e0,0.00789e0,0.00679e0,0.00592e0,0.00524e0,
     0.00468e0};

// input vector y0 is yy
int dso::SGOde::step(double &eps, int &crash) noexcept {
  // ***     begin block 0     ***
  //    check if step size or error tolerance is too small for machine
  //    precision.  if first step, initialize phi array and estimate a
  //    starting step size.

  // if step size is too small, determine an acceptable one
  crash = true;
  if (std::abs(h) < fouru * std::abs(x)) {
    h = std::copysign(fouru * std::abs(x), h);
    return 0;
  }

  // -- break point 5: --
  const double p5eps = 5e-1 * eps;

  //  if error tolerance is too small, increase it to an acceptable value
  const double round = twou * (yy().array() / wt().array()).matrix().norm();
  if (p5eps < round) {
    eps = 2e0*round*(1e0 + fouru);
    return 0;
  }
  // -- break point 15: --
  crash = false;
  g(0) = 1e0;
  g(1) = 5e-1;
  sig(0) = 1e0;
  // if(.not.start) go to 99 TODO
  
  double hold,absh;
  int ifail,kold;
  if (start) {
    // initialize.  compute appropriate step size for first step
    f(x, yy(), yp(), params);
    Phi.col(0) = yp();
    Phi.col(1).setZero();
    const double sum = (yp().array() / wt().array()).matrix().norm();
    absh = std::abs(h);
    if (eps < 16e0 * sum * h *h) absh = 0.25e0 * std::sqrt(eps/sum);
    hold = 0e0;
    k = 1;
    kold = 0;
    start = false;
    phase1 = true;
    nornd = true;
    if (p5eps  <= 1e2 * round) {
      nornd = false;
      Phi.col(14).setZero();
    }
    ifail = 0;
  }

  // ***     end block 0     ***
  
  //                                                                   
  // Repeat blocks 1, 2 (and 3) until step is successful               
  // 
  int step_success = false;
  int kp1,kp2,km1,knew,ns;
  double erkm2,erkm1,erk,xold;
  while (!step_success) {
    // 
    // ***     begin block 1     ***
    //
    // compute coefficients of formulas for this step.  avoid computing
    // those quantities not changed when step size is not changed.
    int kp1 = k + 1;
    int kp2 = k + 2;
    int km1 = k - 1;
    int km2 = k - 2;

    // ns is the number of steps taken with size h, including the current
    // one.  when k.lt.ns, no coefficients change
    if (h != hold)
      ns = 0;
    if (ns <= kold)
      ++ns;
    int nsp1 = ns + 1;
    if (k >= ns) { // this should exit in 199:
      // compute those components of alpha(*),beta(*),psi(*),sig(*) which
      // are changed
      beta(ns - 1) = 1e0;
      alpha(ns - 1) = 1e0 / ns;
      double tmp1 = h * ns;
      sig(nsp1 - 1) = 1e0;
      if (k >= nsp1) { // should exit in 110
        for (int i = nsp1 - 1; i < k; i++) {
          const int im1 = i - 1;
          const double tmp2 = psi(im1);
          psi(im1) = tmp1;
          beta(i) = beta(im1) * psi(im1) / tmp2;
          tmp1 = tmp2 + h;
          alpha(i) = h / tmp1;
          sig(i + 1) = alpha(i) * i * sig(i);
        }
      }
      // -- break point 110: --
      psi(k - 1) = tmp1;

      // compute coefficients g(*)

      // initialize v(*) and set w(*). g(2) is set in data statement
      if (ns <= 1) { // else 120:
        for (int iq = 0; iq < k; iq++) {
          v(iq) = 1e0 / (double)((iq + 1) * (iq + 2));
          // w(iq) = v(iq);
        }
        std::memcpy(w(), v(), sizeof(double) * k);
        // next step is 140:
      } else {
        // aka (ns > 1)
        // if order was raised, update diagonal part of v(*)
        // -- break point 120: --
        if (k > kold) { // else goto 130:
          v(k - 1) = 1e0 / (double)(k * kp1);
          const int nsm2 = ns - 2;
          if (nsm2 >= 1) { // else goto 130:
            for (int j = 0; j < ns - 2; j++) {
              int i = k - j - 1;
              // TODO is indexing correct here ?
              v(i) -= alpha(j) * v(i + 1);
            }
          }
        }

        // update v(*) and set w(*)
        // -- break point 130: --
        const int limit1 = kp1 - ns;
        const double tmp5 = alpha(ns - 1);
        for (int iq = 0; iq < limit1; iq++) {
          v(iq) -= tmp5 * v(iq + 1);
          // w(iq) = v(iq);
        }
        std::memcpy(w(), v(), sizeof(double) * limit1);
        g(nsp1 - 1) = w(0);
      }

      // compute the g(*) in the work vector w(*)
      // -- break point 140: --
      int nsp2 = ns + 2;
      if (kp1 >= nsp2) { // else goto 199:
        for (int i = nsp2; i <= kp1; i++) {
          const int limit2 = kp2 - i;
          const double tmp6 = alpha(i - 2);
          for (int iq = 0; iq < limit2; iq++) {
            w(iq) -= tmp6 * w(iq + 1);
          }
          g(i - 1) = w(0);
        }
      }
    }
    // -- break point 199: --
    // ***     end block 1     **

    // ***     begin block 2     ***
    // predict a solution p(*), evaluate derivatives using predicted
    // solution, estimate local error at order k and errors at orders k,
    // k-1, k-2 as if constant step size were used.
    // ***

    // change phi to phi star
    if (k >= nsp1) { // else goto 215:
      for (int i = nsp1; i < k; i++) {
        const double b = beta(i - 1);
        for (int l = 0; l < neqn; l++) {
          Phi(l, i - 1) = b * Phi(l, i - 1);
        }
      }
    }

    // predict solution and differences
    // -- break point 215: --
    Phi.col(kp2 - 1) = Phi.col(kp1 - 1);
    Phi.col(kp1 - 1).setZero();
    this->p().setZero();
    // -- break point 220: --
    for (int j = 1; j <= k; j++) {
      const int i = kp1 - j;
      const int ip1 = i + 1;
      const double gi = g(i - 1);
      p() += gi * Phi.col(i - 1);
      Phi.col(i - 1) += Phi.col(ip1 - 1);
    }
    // -- break point 230: --
    if (!nornd) { // else goto 240:
      const auto tau = h * p() - Phi.col(14);
      p() = yy() + tau;
      Phi.col(15) = (p() - yy()) - tau;
      // goto 250:
    } else {
      // -- break point 240: --
      p() = yy() + h * p();
    }
    // -- break point 250: --
    xold = x;
    x += h;
    absh = std::abs(h);
    f(x, p(), yp(), params);

    // estimate errors at orders k,k-1,k-2
    erkm2 = 0e0;
    erkm1 = 0e0;
    erk = 0e0;
    for (int l = 0; l < neqn; l++) {
      const double t3 = 1e0 / wt()(l);
      const double t4 = yp()(l) - Phi(l, 0);
      if (km2 > 0)
        erkm2 += std::pow((Phi(l, km1 - 1) + t4) * t3, 2);
      if (km2 >= 0)
        erkm1 += std::pow((Phi(l, k - 1) + t4) * t3, 2);
      erk += (t4 * t3) * (t4 * t3);
    }
    if (km2 > 0)
      erkm2 = absh * sig(km1 - 1) * gstr[km2 - 1] * std::sqrt(erkm2);
    if (km2 >= 0)
      erkm1 = absh * sig(k - 1) * gstr[km1 - 1] * std::sqrt(erkm1);
    const double t5 = absh * std::sqrt(erk);
    const double err = t5 * (g(k - 1) - g(kp1 - 1));
    erk = t5 * sig(kp1 - 1) * gstr[k - 1];
    knew = k;

    // test if order should be lowered
    if (km2 > 0) {
      if (std::max(erkm1, erkm2) <= erk)
        knew = km1;
      // exit at 299:
    } else if (km2 == 0) {
      if (erkm1 <= 5e-1 * erk)
        knew = km1;
    }
    // -- break point 299: --
    step_success = (err <= eps);
    //
    // ***     end block 2     ***
    //

    //
    // ***     begin block 3     ***
    // the step is unsuccessful.  restore  x, phi(*,*), psi(*) .
    // if third consecutive failure, set order to one.  if step fails more
    // than three times, consider an optimal step size.  double error
    // tolerance and return if estimated step size is too small for machine
    // precision.
    //                 ***
    if (!step_success) {
      // restore x, phi(*,*) and psi(*)
      phase1 = false;
      x = xold;
      for (int i = 0; i < k; i++) {
        const double t1 = 1e0 / beta(i);
        Phi.col(i) = t1 * (Phi.col(i) - Phi.col(i + 1));
      }
      if (k >= 2) { // else goto 320:
        for (int i = 1; i < k; i++)
          psi(i - 1) = psi(i) - h;
      }

      // on third failure, set order to one.  thereafter, use optimal step
      // size
      // -- break point 320: --
      ++ifail;
      double t2 = 5e-1;
      if (ifail - 3 > 0 && p5eps < 0.25 * erk)
        t2 = std::sqrt(p5eps / erk);
      if (ifail - 3 >= 0)
        knew = 1;
      h *= t2;
      k = knew;
      if (!(std::abs(h) >= fouru * std::abs(x))) {
        crash = true;
        h = std::copysign(fouru * std::abs(x), h);
        eps += eps;
        return 1;
      }
      // goto 100:
    }
    //
    // ***     end block 3     ***
    //
  }

  //
  // ***    begin block 4     ***
  //
  // the step is successful.  correct the predicted solution, evaluate
  // the derivatives using the corrected solution and update the
  // differences.  determine best order and step size for next step.
  // 
  // -- break point 400: --
  kold = k;
  hold = h;

  // correct and evaluate
  const double t1 = h * g(kp1-1);
  if (!nornd) {
    const auto rho = t1 * (yp() - Phi.col(0)) - Phi.col(15);
    yy() = p() + rho;
    Phi.col(14) = (yy() - p()) - rho;
  } else {
    yy() = p() + t1 * (yp() - Phi.col(0));
  }
  // -- break point 420: --
  f(x,yy(),yp(),params);

  // update differences for next step
  Phi.col(kp1-1) = yp() - Phi.col(0);
  Phi.col(kp2-1) = Phi.col(kp1-1) - Phi.col(kp2-1);
  for (int i=0; i<k; i++)
    Phi.col(i) += Phi.col(kp1-1);

  // estimate error at order k+1 unless:
  //   in first phase when always raise order,
  //   already decided to lower order,
  //   step size not constant so estimate unreliable
  double erkp1 = 0e0;
  if (knew == km1 || k == 12) phase1 = false;
  
  if (phase1) {
    // 450:
    k = kp1;
    erk = erkp1;
    // goto 460:
  } else if (knew == km1) {
    // 455:
    k = km1;
    erk = erkp1;
    // goto 460
  } else if (kp1 > ns) {
    ;
    // goto 460
  } else {
    for (int l=0; l<neqn; l++)
      erkp1 += std::pow(Phi(l,kp2-1) / wt()(l), 2);
    erkp1 = absh * gstr[kp1-1] * std::sqrt(erkp1);
    // using estimated error at order k+1, determine appropriate order
    // for next step
    if (k > 1) {
      if (erkm1 <= std::min(erk,erkp1)) {
        // 455:
         k = km1;
         erk = erkm1;
         // goto 460:
      }
    } else if (erkp1 >= 0.5e0*erk) {
      ;
      // goto 460:
    } else {
      // 450:
      k = kp1;
      erk = erkp1;
    }
  }

  // with new order determine appropriate step size for next step
  // --break point 460: --
  double hnew = h + h;
  if (!(phase1 || p5eps >= erk*two[k])) {
    hnew = h;
    if (p5eps < erk) {
      const double t2 = k+1;
      const double r = std::pow(p5eps/erk, 1e0/t2);
      hnew = absh*std::max(0.5e0,std::min(0.9e0,r));
      hnew = std::copysign(std::max(hnew,fouru*std::abs(x)),h);
    }
  }
  // --break point 465: --
  h = hnew;
  return 0;
}
