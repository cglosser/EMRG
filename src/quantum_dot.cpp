#include "quantum_dot.h"

QuantumDot::QuantumDot(const Eigen::Vector3d &pos, const double freq,
                       const std::pair<double, double> &damping,
                       const Eigen::Vector3d &dip)
    : pos(pos), freq(freq), damping(damping), dip(dip)
{
}

matrix_elements QuantumDot::liouville_rhs(const matrix_elements &rho,
                                          const cmplx rabi, const double laser_freq) const
{
  const cmplx m0 = -iu * (rabi * std::conj(rho[1]) - std::conj(rabi) * rho[1]) -
                   (rho[0] - 1.0) / damping.first;
  const cmplx m1 =
      -iu * (rabi * (1.0 - 2.0 * rho[0]) + rho[1] * (laser_freq - freq)) -
      rho[1] / damping.second;
  return matrix_elements(m0, m1);
}

Eigen::Vector3d separation(const QuantumDot &d1, const QuantumDot &d2)
{
  return d2.pos - d1.pos;
}

std::ostream &operator<<(std::ostream &os, const QuantumDot &qd)
{
  os << qd.pos.transpose() << " " << qd.freq << " " << qd.damping.first << " "
     << qd.damping.second << " " << qd.dip.transpose();
  return os;
}

std::istream &operator>>(std::istream &is, QuantumDot &qd)
{
  is >> qd.pos[0] >> qd.pos[1] >> qd.pos[2] >> qd.freq >> qd.damping.first >>
      qd.damping.second >> qd.dip[0] >> qd.dip[1] >> qd.dip[2];
  return is;
}

DotVector import_dots(const std::string &fname)
{
  std::ifstream ifs(fname);
  if(!ifs) throw std::runtime_error("Could not open " + fname);

  std::istream_iterator<QuantumDot> in_iter(ifs), eof;
  return DotVector(in_iter, eof);
}

typedef std::vector<
    std::function<matrix_elements(const matrix_elements &, const cmplx)>>
    rhs_func_vector;
rhs_func_vector rhs_functions(const DotVector &dots, const double laser_frequency)
{
  rhs_func_vector funcs(dots.size());

  using std::placeholders::_1;
  using std::placeholders::_2;
  std::transform(dots.begin(), dots.end(), funcs.begin(),
                 [laser_frequency](const QuantumDot &d) {
                   return std::bind(&QuantumDot::liouville_rhs, d, _1, _2,
                                    laser_frequency);
                 });
  return funcs;
}
