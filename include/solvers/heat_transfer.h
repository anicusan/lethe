/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2019 - 2020 by the Lethe authors
 *
 * This file is part of the Lethe library
 *
 * The Lethe library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE at
 * the top level of the Lethe distribution.
 *
 * ---------------------------------------------------------------------

 *
 * Implementation of heat transfer as an auxiliary physics.
 * This heat equation is weakly coupled to the velocity field.
 * Equation solved:
 * rho * Cp * (dT/dt + u.gradT) = k div(gradT) + nu/rho * (gradu : gradu)
 *
 * Polytechnique Montreal, 2020-
 */

#ifndef lethe_heat_transfer_h
#define lethe_heat_transfer_h

#include <core/bdf.h>
#include <core/simulation_control.h>

#include <solvers/auxiliary_physics.h>
#include <solvers/heat_transfer_assemblers.h>
#include <solvers/heat_transfer_scratch_data.h>
#include <solvers/multiphysics_interface.h>

#include <deal.II/base/convergence_table.h>
#include <deal.II/base/quadrature_lib.h>

#include <deal.II/distributed/solution_transfer.h>
#include <deal.II/distributed/tria_base.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_simplex_p.h>
#include <deal.II/fe/mapping_fe.h>
#include <deal.II/fe/mapping_q.h>

#include <deal.II/lac/trilinos_sparse_matrix.h>
#include <deal.II/lac/trilinos_vector.h>


template <int dim>
class HeatTransfer : public AuxiliaryPhysics<dim, TrilinosWrappers::MPI::Vector>
{
public:
  HeatTransfer<dim>(MultiphysicsInterface<dim> *     multiphysics_interface,
                    const SimulationParameters<dim> &p_simulation_parameters,
                    std::shared_ptr<parallel::DistributedTriangulationBase<dim>>
                                                       p_triangulation,
                    std::shared_ptr<SimulationControl> p_simulation_control)
    : AuxiliaryPhysics<dim, TrilinosWrappers::MPI::Vector>(
        p_simulation_parameters.non_linear_solver)
    , multiphysics(multiphysics_interface)
    , simulation_parameters(p_simulation_parameters)
    , triangulation(p_triangulation)
    , simulation_control(p_simulation_control)
    , dof_handler(*triangulation)

  {
    if (simulation_parameters.mesh.simplex)
      {
        // for simplex meshes
        fe = std::make_shared<FE_SimplexP<dim>>(
          simulation_parameters.fem_parameters.temperature_order);
        temperature_mapping = std::make_shared<MappingFE<dim>>(*fe);
        cell_quadrature = std::make_shared<QGaussSimplex<dim>>(fe->degree + 1);
        face_quadrature =
          std::make_shared<QGaussSimplex<dim - 1>>(fe->degree + 1);
      }
    else
      {
        // Usual case, for quad/hex meshes
        fe = std::make_shared<FE_Q<dim>>(
          simulation_parameters.fem_parameters.temperature_order);
        temperature_mapping = std::make_shared<MappingQ<dim>>(
          fe->degree, simulation_parameters.fem_parameters.qmapping_all);
        cell_quadrature = std::make_shared<QGauss<dim>>(fe->degree + 1);
        face_quadrature = std::make_shared<QGauss<dim - 1>>(fe->degree + 1);
      }

    // Allocate solution transfer
    solution_transfer =
      std::make_shared<parallel::distributed::
                         SolutionTransfer<dim, TrilinosWrappers::MPI::Vector>>(
        dof_handler);

    // Set size of previous solutions using BDF schemes information
    previous_solutions.resize(maximum_number_of_previous_solutions());

    // Prepare previous solutions transfer
    previous_solutions_transfer.reserve(previous_solutions.size());
    for (unsigned int i = 0; i < previous_solutions.size(); ++i)
      {
        previous_solutions_transfer.emplace_back(
          parallel::distributed::
            SolutionTransfer<dim, TrilinosWrappers::MPI::Vector>(
              this->dof_handler));
      }
  }

  /**
   * @brief Call for the assembly of the matrix and the right-hand side.
   *
   * @deprecated This function is to be deprecated when the new assembly mechanism
   * is integrated to this solver
   */
  void
  assemble_matrix_and_rhs();

  /**
   * @brief Call for the assembly of the right-hand side
   *
   * @deprecated This function is to be deprecated when the new assembly mechanism
   * is integrated to this solver
   */
  void
  assemble_rhs();

  /**
   * @brief Call for the assembly of the matrix and the right-hand side of the Nitsche restriction for the heat transfert equation.
   *
   * @param assemble_matrix boolean that is true for matrix assembly, and false for rhs assembly
   */
  void
  assemble_nitsche_heat_restriction(bool assemble_matrix);

  /**
   * @brief Attach the solution vector to the DataOut provided. This function
   * enable the auxiliary physics to output their solution via the core solver.
   */
  void
  attach_solution_to_output(DataOut<dim> &data_out) override;


  /**
   * @brief Calculates the L2 error of the solution
   */
  double
  calculate_L2_error();


  /**
   * @brief Carry out the operations required to finish a simulation correctly.
   */
  void
  finish_simulation() override;

  /**
   * @brief Rearrange vector solution correctly for transient simulations
   */
  void
  percolate_time_vectors() override;

  /**
   * @brief Postprocess the auxiliary physics results. Post-processing this case implies
   * the calculation of all derived quantities using the solution vector of the
   * physics. It does not concern the output of the solution using the
   * DataOutObject, which is accomplished through the attach_solution_to_output
   * function
   */
  void
  postprocess(bool first_iteration) override;


  /**
   * @brief pre_mesh_adaption Prepares the auxiliary physics variables for a
   * mesh refinement/coarsening
   */
  void
  pre_mesh_adaptation();

  /**
   * @brief post_mesh_adaption Interpolates the auxiliary physics variables to the new mesh
   */
  void
  post_mesh_adaptation();

  /**
   * @brief Compute the Kelly error estimator for mesh refinement.
   *
   * @param ivar The current element of the map simulation_parameters.mesh_adaptation.variables
   *
   * @param estimated_error_per_cell The deal.II vector of estimated_error_per_cell
   */
  void
  compute_kelly(const std::pair<const Parameters::MeshAdaptation::Variable,
                                Parameters::MultipleAdaptationParameters> &ivar,
                dealii::Vector<float> &estimated_error_per_cell);

  /**
   * @brief Prepares Heat Transfer to write checkpoint
   */
  void
  write_checkpoint() override;

  /**
   * @brief Allows Heat Transfer to set-up solution vector from checkpoint file;
   */
  void
  read_checkpoint() override;

  /**
   * @brief Sets-up the DofHandler and the degree of freedom associated with the physics.
   */
  void
  setup_dofs() override;

  /**
   * @brief Sets-up the initial conditions associated with the physics. Generally, physics
   * only support imposing nodal values, but some physics additionnaly support
   * the use of L2 projection or steady-state solutions.
   */
  void
  set_initial_conditions() override;

  /**
   * @brief Call for the solution of the linear system of equation using a strategy appropriate
   * to the auxiliary physics
   *
   * @param initial_step Provides the linear solver with indication if this solution is the first
   * one for the system of equation or not
   *
   * @param renewed_matrix Indicates to the linear solve if the system matrix has been recalculated or not
   */
  void
  solve_linear_system(const bool initial_step,
                      const bool renewed_matrix = true);

  /**
   * @brief Getter methods to get the private attributes for the physic currently solved
   * NB : dof_handler and present_solution are passed to the multiphysics
   * interface at the end of the setup_dofs method
   */
  const DoFHandler<dim> &
  get_dof_handler() override
  {
    return dof_handler;
  }
  TrilinosWrappers::MPI::Vector &
  get_evaluation_point() override
  {
    return evaluation_point;
  }
  TrilinosWrappers::MPI::Vector &
  get_local_evaluation_point() override
  {
    return local_evaluation_point;
  }
  TrilinosWrappers::MPI::Vector &
  get_newton_update() override
  {
    return newton_update;
  }
  TrilinosWrappers::MPI::Vector &
  get_present_solution() override
  {
    return present_solution;
  }
  TrilinosWrappers::MPI::Vector &
  get_system_rhs() override
  {
    return system_rhs;
  }
  AffineConstraints<double> &
  get_nonzero_constraints() override
  {
    return nonzero_constraints;
  }


private:
  /**
   *  @brief Assembles the matrix associated with the solver
   */
  void

  assemble_system_matrix();

  /**
   * @brief Assemble the rhs associated with the solver
   */
  void
  assemble_system_rhs();


  /**
   * @brief Assemble the local matrix for a given cell.
   *
   * This function is used by the WorkStream class to assemble
   * the system matrix. It is a thread safe function.
   *
   * @param cell The cell for which the local matrix is assembled.
   *
   * @param scratch_data The scratch data which is used to store
   * the calculated finite element information at the gauss point.
   * See the documentation for HeatTransferScratchData for more
   * information
   *
   * @param copy_data The copy data which is used to store
   * the results of the assembly over a cell
   */
  virtual void
  assemble_local_system_matrix(
    const typename DoFHandler<dim>::active_cell_iterator &cell,
    HeatTransferScratchData<dim> &                        scratch_data,
    StabilizedMethodsCopyData &                           copy_data);

  /**
   * @brief Assemble the local rhs for a given cell
   *
   * @param cell The cell for which the local matrix is assembled.
   *
   * @param scratch_data The scratch data which is used to store
   * the calculated finite element information at the gauss point.
   * See the documentation for HeatTransferScratchData for more
   * information
   *
   * @param copy_data The copy data which is used to store
   * the results of the assembly over a cell
   */
  virtual void
  assemble_local_system_rhs(
    const typename DoFHandler<dim>::active_cell_iterator &cell,
    HeatTransferScratchData<dim> &                        scratch_data,
    StabilizedMethodsCopyData &                           copy_data);

  /**
   * @brief sets up the vector of assembler functions
   */
  virtual void
  setup_assemblers();


  /**
   * @brief Copy local cell information to global matrix
   */

  virtual void
  copy_local_matrix_to_global_matrix(
    const StabilizedMethodsCopyData &copy_data);

  /**
   * @brief Copy local cell rhs information to global rhs
   */

  virtual void
  copy_local_rhs_to_global_rhs(const StabilizedMethodsCopyData &copy_data);

  MultiphysicsInterface<dim> *     multiphysics;
  const SimulationParameters<dim> &simulation_parameters;


  // Core elements for the heat transfer simulation
  std::shared_ptr<parallel::DistributedTriangulationBase<dim>> triangulation;
  std::shared_ptr<SimulationControl> simulation_control;
  DoFHandler<dim>                    dof_handler;

  std::shared_ptr<FiniteElement<dim>> fe;
  ConvergenceTable                    error_table;

  // Mapping and Quadrature
  std::shared_ptr<Mapping<dim>>        temperature_mapping;
  std::shared_ptr<Quadrature<dim>>     cell_quadrature;
  std::shared_ptr<Quadrature<dim - 1>> face_quadrature;


  // Solution storage:
  IndexSet locally_owned_dofs;
  IndexSet locally_relevant_dofs;

  TrilinosWrappers::MPI::Vector  evaluation_point;
  TrilinosWrappers::MPI::Vector  local_evaluation_point;
  TrilinosWrappers::MPI::Vector  newton_update;
  TrilinosWrappers::MPI::Vector  present_solution;
  TrilinosWrappers::MPI::Vector  system_rhs;
  AffineConstraints<double>      nonzero_constraints;
  AffineConstraints<double>      zero_constraints;
  TrilinosWrappers::SparseMatrix system_matrix;


  // Previous solutions vectors
  std::vector<TrilinosWrappers::MPI::Vector> previous_solutions;
  std::vector<TrilinosWrappers::MPI::Vector> solution_stages;

  // Solution transfer classes
  std::shared_ptr<
    parallel::distributed::SolutionTransfer<dim, TrilinosWrappers::MPI::Vector>>
    solution_transfer;
  std::vector<
    parallel::distributed::SolutionTransfer<dim, TrilinosWrappers::MPI::Vector>>
    previous_solutions_transfer;

  // Reference for GGLS https://onlinelibrary.wiley.com/doi/abs/10.1002/nme.2324
  // Warning, this GGLS implementation is valid only for Linear elements
  // Quad elements will be lacking the third derivative of the diffusion
  // operator Whether this affects or not the final result is unclear to me at
  // the moment. Additionnaly, this formulation does not use the gradient of the
  // source term. The same applies, I have no clue if this is detrimental or not
  // to the solution since anyway the GGLS term scales as h^(order+1)
  const bool GGLS = true;

  // Assemblers for the matrix and rhs
  std::vector<std::shared_ptr<HeatTransferAssemblerBase<dim>>> assemblers;
};


#endif
