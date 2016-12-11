#ifndef INTEGRATOR_H
#define INTEGRATOR_H

#include <Eigen/Dense>
#include <algorithm>
#include <boost/multi_array.hpp>
#include <complex>
#include <vector>

#include "math_utils.h"

namespace PredictorCorrector {
  class Weights;
  class Integrator;
}

class PredictorCorrector::Weights {
 public:
  Weights(const int, const int, const double);

  Eigen::ArrayXXd ps, cs;
  double future_coef;

  int width() const { return n_time; }
 private:
  int n_time;
};

class PredictorCorrector::Integrator {
 public:
  typedef Eigen::Vector2cd soltype;
  Integrator(const int, const int, const double, const int, const int,
             const double);
  void step();

 private:
  typedef boost::multi_array<soltype, 3>
      HistoryArray;

  int now;
  int num_solutions, num_steps;
  double dt;
  Weights weights;
  HistoryArray history;

  void predictor();
  void evaluator();
  void corrector();
};

#endif
