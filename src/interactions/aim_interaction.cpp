#include "aim_interaction.h"

AIM::Grid::Grid(const Eigen::Array3d &spacing,
                const std::shared_ptr<DotVector> &dots,
                const int pad)
    : spacing(spacing), dots(dots), padding(pad), bounds(calculate_bounds())
{
  dimensions = bounds.col(1) - bounds.col(0);
  num_boxes = dimensions.prod();
  max_diagonal = (dimensions.cast<double>() * spacing).matrix().norm();
  boxes.resize(dimensions.prod());

  sort_points_on_boxidx();
  map_points_to_boxes();
}

AIM::Grid::Grid(const Eigen::Array3d &spacing,
                const std::shared_ptr<DotVector> &dots)
    : Grid(spacing, dots, 0)
{
}

AIM::Grid::BoundsArray AIM::Grid::calculate_bounds() const
{
  BoundsArray b;
  b.setZero();

  for(const auto &qdot : *dots) {
    Eigen::Vector3i grid_coord = grid_coordinate(qdot.position());

    b.col(0) = grid_coord.array().min(b.col(0));
    b.col(1) = grid_coord.array().max(b.col(1));
  }

  b.col(0) -= padding;
  b.col(1) += padding + 1;
  // The +1 ensures a grid of boxes entirely contains the qdot coordinates

  return b;
}

Eigen::Vector3i AIM::Grid::grid_coordinate(const Eigen::Vector3d &coord) const
{
  return floor(coord.cwiseQuotient(spacing.matrix()).array()).cast<int>();
}

size_t AIM::Grid::coord_to_idx(const Eigen::Vector3i &coord) const
{
  Eigen::Vector3i shifted(coord - bounds.col(0).matrix());

  return shifted(0) + dimensions(0) * (shifted(1) + dimensions(1) * shifted(2));
}

Eigen::Vector3i AIM::Grid::idx_to_coord(size_t idx) const
{
  const int nxny = dimensions(0) * dimensions(1);
  const int z = idx / nxny;
  idx -= z * nxny;
  const int y = idx / dimensions(0);
  const int x = idx % dimensions(0);

  return Eigen::Vector3i(x, y, z);
}

Eigen::Vector3d AIM::Grid::spatial_coord_of_box(const size_t box_id) const
{
  const Eigen::Vector3d r =
      (idx_to_coord(box_id).cast<double>().array() * spacing);
  return r + bounds.col(0).cast<double>().matrix();
}

std::vector<size_t> AIM::Grid::expansion_box_indices(const Eigen::Vector3d &pos,
                                                     const int order) const
{
  Eigen::Vector3i origin = grid_coordinate(pos);
  std::vector<size_t> indices(std::pow(order + 1, 3));

  size_t idx = 0;
  for(int nx = 0; nx <= order; ++nx) {
    for(int ny = 0; ny <= order; ++ny) {
      for(int nz = 0; nz <= order; ++nz) {
        const Eigen::Vector3i delta(grid_sequence(nx), grid_sequence(ny),
                                    grid_sequence(nz));
        const size_t grid_idx = coord_to_idx(origin + delta);

        indices.at(idx++) = grid_idx;
      }
    }
  }

  return indices;
}

void AIM::Grid::sort_points_on_boxidx() const
{
  auto grid_comparitor = [&](const QuantumDot &q1, const QuantumDot &q2) {
    return coord_to_idx(grid_coordinate(q1.position())) <
           coord_to_idx(grid_coordinate(q2.position()));
  };

  std::stable_sort(dots->begin(), dots->end(), grid_comparitor);
}

void AIM::Grid::map_points_to_boxes()
{
  for(size_t box_idx = 0; box_idx < boxes.size(); ++box_idx) {
    auto IsInBox = [=](const QuantumDot &qd) {
      return coord_to_idx(grid_coordinate(qd.position())) == box_idx;
    };

    auto begin = std::find_if(dots->begin(), dots->end(), IsInBox);
    auto end = std::find_if_not(begin, dots->end(), IsInBox);
    boxes.at(box_idx) = std::make_pair(begin, end);
  }
}

AIM::AimInteraction::AimInteraction(const std::shared_ptr<DotVector> &dots,
                                    const Eigen::Vector3d &spacing,
                                    const int interp_order,
                                    const double c,
                                    const double dt)
    : Interaction(dots),
      grid(spacing, dots),
      interp_order(interp_order),
      c(c),
      dt(dt),
      fourier_table(boost::extents[CmplxArray::extent_range(
          1, grid.max_transit_steps(c, dt))][grid.dimensions(0)]
                                  [grid.dimensions(1)][grid.dimensions(2) + 1])
{
  fill_fourier_table();
}

const Interaction::ResultArray &AIM::AimInteraction::evaluate(const int step)
{
  results = 0;
  return results;
}

void AIM::AimInteraction::fill_fourier_table()
{
  const int max_transit_steps = grid.max_transit_steps(c, dt);

  SpacetimeArray<double> g_mat(
      boost::extents[SpacetimeArray<double>::extent_range(1, max_transit_steps)]
                    [grid.dimensions(0)][grid.dimensions(1)]
                    [2 * grid.dimensions(2)]);

  // Set up FFTW plan; will transform real-valued Toeplitz matrix to the
  // positive frequency complex-valued FFT values (known to be conjugate
  // symmetric to eliminate redundancy).

  const int len[] = {2 * grid.dimensions(2)};
  const int howmany = std::accumulate(g_mat.shape(), g_mat.shape() + 3, 1,
                                      std::multiplies<int>());
  const int idist = 2 * grid.dimensions(2), odist = grid.dimensions(2) + 1;
  const int istride = 1, ostride = 1;
  const int *inembed = len, *onembed = len;

  fftw_plan circulant_plan;
  circulant_plan = fftw_plan_many_dft_r2c(
      1, len, howmany, g_mat.data(), inembed, istride, idist,
      reinterpret_cast<fftw_complex *>(fourier_table.data()), onembed, ostride,
      odist, FFTW_MEASURE);

  std::fill(g_mat.data(), g_mat.data() + g_mat.num_elements(), 0);

  fill_gmatrix_table(g_mat);

  // Transform the circulant vectors into their equivalently-diagonal
  // representation. Buckle up.

  fftw_execute(circulant_plan);
}

void AIM::AimInteraction::fill_gmatrix_table(
    SpacetimeArray<double> &gmatrix_table) const
{
  // Build the circulant vectors that define the G "matrices." Since the G
  // matrices are Toeplitz (and symmetric), they're uniquely determined by
  // their first row. The first row gets computed here then mirrored to make a
  // list of every circulant (and thus FFT-able) vector. This function needs to
  // accept a non-const reference to a SpacetimeArray (instead of just
  // returning such an array) to play nice with FFTW and its workspaces.

  Interpolation::UniformLagrangeSet interp(interp_order);
  for(int nx = 0; nx < grid.dimensions(0); ++nx) {
    for(int ny = 0; ny < grid.dimensions(1); ++ny) {
      for(int nz = 0; nz < grid.dimensions(2); ++nz) {
        const size_t box_idx = grid.coord_to_idx(Eigen::Vector3i(nx, ny, nz));
        if(box_idx == 0) continue;

        const auto dr =
            grid.spatial_coord_of_box(box_idx) - grid.spatial_coord_of_box(0);

        const double arg = dr.norm() / (c * dt);
        const std::pair<int, double> split_arg = split_double(arg);

        // Do the time-axis last; we can share a lot of work
        for(int time_idx = 1;
            time_idx < static_cast<int>(gmatrix_table.shape()[0]); ++time_idx) {
          const int polynomial_idx = static_cast<int>(ceil(time_idx - arg));

          if(0 <= polynomial_idx && polynomial_idx <= interp_order) {
            interp.evaluate_derivative_table_at_x(split_arg.second, dt);
            gmatrix_table[time_idx][nx][ny][nz] =
                interp.evaluations[0][polynomial_idx];

            if(nz != 0) {  // Make the circulant "mirror"
              gmatrix_table[time_idx][nx][ny][2 * grid.dimensions(2) - nz] =
                  gmatrix_table[time_idx][nx][ny][nz];
            }
          }
        }
      }
    }
  }
}
