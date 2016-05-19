/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "idas_interface.hpp"

#include "casadi/core/std_vector_tools.hpp"

#ifdef WITH_SYSTEM_SUNDIALS
#include <external_packages/sundials-2.5mod/idas/idas_spils_impl.h>
#endif

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_INTEGRATOR_IDAS_EXPORT
      casadi_register_integrator_idas(Integrator::Plugin* plugin) {
    plugin->creator = IdasInterface::creator;
    plugin->name = "idas";
    plugin->doc = IdasInterface::meta_doc.c_str();
    plugin->version = 30;
    return 0;
  }

  extern "C"
  void CASADI_INTEGRATOR_IDAS_EXPORT casadi_load_integrator_idas() {
    Integrator::registerPlugin(casadi_register_integrator_idas);
  }

  IdasInterface::IdasInterface(const std::string& name, const Function& dae)
    : SundialsInterface(name, dae) {
  }

  IdasInterface::~IdasInterface() {
    clear_memory();
  }

  Options IdasInterface::options_
  = {{&SundialsInterface::options_},
     {{"suppress_algebraic",
       {OT_BOOL,
        "Suppress algebraic variables in the error testing"}},
      {"calc_ic",
       {OT_BOOL,
        "Use IDACalcIC to get consistent initial conditions."}},
      {"calc_icB",
       {OT_BOOL,
        "Use IDACalcIC to get consistent initial conditions for "
        "backwards system [default: equal to calc_ic]."}},
      {"abstolv",
       {OT_DOUBLEVECTOR,
        "Absolute tolerarance for each component"}},
      {"fsens_abstolv",
       {OT_DOUBLEVECTOR,
        "Absolute tolerarance for each component, forward sensitivities"}},
      {"max_step_size",
       {OT_DOUBLE,
        "Maximim step size"}},
      {"first_time",
       {OT_DOUBLE,
        "First requested time as a fraction of the time interval"}},
      {"cj_scaling",
       {OT_BOOL,
        "IDAS scaling on cj for the user-defined linear solver module"}},
      {"extra_fsens_calc_ic",
       {OT_BOOL,
        "Call calc ic an extra time, with fsens=0"}},
      {"init_xdot",
       {OT_DOUBLEVECTOR,
        "Initial values for the state derivatives"}}
     }
  };

  void IdasInterface::init(const Dict& opts) {
    log("IdasInterface::init", "begin");

    // Call the base class init
    SundialsInterface::init(opts);

    // Default options
    cj_scaling_ = false;
    calc_ic_ = true;
    suppress_algebraic_ = false;
    max_step_size_ = 0;

    // Read options
    for (auto&& op : opts) {
      if (op.first=="init_xdot") {
        init_xdot_ = op.second;
      } else if (op.first=="cj_scaling") {
        cj_scaling_ = op.second;
      } else if (op.first=="calc_ic") {
        calc_ic_ = op.second;
      } else if (op.first=="suppress_algebraic") {
        suppress_algebraic_ = op.second;
      } else if (op.first=="max_step_size") {
        max_step_size_ = op.second;
      } else if (op.first=="abstolv") {
        abstolv_ = op.second;
      } else if (op.first=="fsens_abstolv") {
        fsens_abstolv_ = op.second;
      }
    }

    // Default dependent options
    calc_icB_ = calc_ic_;
    first_time_ = grid_.back();

    // Read dependent options
    for (auto&& op : opts) {
      if (op.first=="calc_icB") {
        calc_icB_ = op.second;
      } else if (op.first=="first_time") {
        first_time_ = op.second;
      }
    }

    create_function("daeF", {"x", "z", "p", "t"}, {"ode", "alg"});
    create_function("quadF", {"x", "z", "p", "t"}, {"quad"});
    create_function("daeB", {"rx", "rz", "rp", "x", "z", "p", "t"},
                            {"rode", "ralg"});
    create_function("quadB", {"rx", "rz", "rp", "x", "z", "p", "t"},
                             {"rquad"});

    // Create a Jacobian if requested
    if (exact_jacobian_) {
      set_function(oracle_.is_a("sxfunction") ? getJacF<SX>() : getJacF<MX>());
      init_linsol();
    }

    // Create a backwards Jacobian if requested
    if (exact_jacobianB_ && nrx_>0) {
      set_function(oracle_.is_a("sxfunction") ? getJacB<SX>() : getJacB<MX>());
      init_linsolB();
    }

    // Get initial conditions for the state derivatives
    if (init_xdot_.empty()) {
      init_xdot_.resize(nx_, 0);
    } else {
      casadi_assert_message(
        init_xdot_.size()==nx_,
        "Option \"init_xdot\" has incorrect length. Expecting " << nx_
        << ", but got " << init_xdot_.size()
        << ". Note that this message may actually be generated by the augmented"
        " integrator. In that case, make use of the 'augmented_options' options"
        " to correct 'init_xdot' for the augmented integrator.");
    }

    // Attach functions for jacobian information
    if (exact_jacobian_) {
      switch (linsol_f_) {
      case SD_ITERATIVE:
        create_function("jtimesF",
          {"t", "x", "z", "p", "fwd:x", "fwd:z"},
          {"fwd:ode", "fwd:alg"});
        break;
      default: break;
      }
    }

    if (exact_jacobianB_) {
      switch (linsol_g_) {
      case SD_ITERATIVE:
        create_function("jtimesB",
          {"t", "x", "z", "p", "rx", "rz", "rp", "fwd:rx", "fwd:rz"},
          {"fwd:rode", "fwd:ralg"});
        break;
      default: break;
      }
    }

    log("IdasInterface::init", "end");
  }

  void IdasInterface::initTaping(IdasMemory* m) const {
    casadi_assert(!m->isInitTaping);

    // Get the interpolation type
    int interpType;
    if (interpolation_type_=="hermite") {
      interpType = IDA_HERMITE;
    } else if (interpolation_type_=="polynomial") {
      interpType = IDA_POLYNOMIAL;
    } else {
      casadi_error("\"interpolation_type\" must be \"hermite\" or \"polynomial\"");
    }

    // Initialize adjoint sensitivities
    int flag = IDAAdjInit(m->mem, steps_per_checkpoint_, interpType);
    if (flag != IDA_SUCCESS) idas_error("IDAAdjInit", flag);

    m->isInitTaping = true;
  }

  int IdasInterface::res(double t, N_Vector xz, N_Vector xzdot,
                                N_Vector rr, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(xz);
      m->arg[1] = NV_DATA_S(xz)+s.nx_;
      m->arg[2] = m->p;
      m->arg[3] = &t;
      m->res[0] = NV_DATA_S(rr);
      m->res[1] = NV_DATA_S(rr)+s.nx_;
      s.calc_function(m, "daeF");

      // Subtract state derivative to get residual
      casadi_axpy(s.nx_, -1., NV_DATA_S(xzdot), NV_DATA_S(rr));
      return 0;
    } catch(int flag) { // recoverable error
      return flag;
    } catch(exception& e) { // non-recoverable error
      userOut<true, PL_WARN>() << "res failed: " << e.what() << endl;
      return -1;
    }
  }

  void IdasInterface::ehfun(int error_code, const char *module, const char *function,
                                   char *msg, void *eh_data) {
    try {
      auto m = to_mem(eh_data);
      auto& s = m->self;
      userOut<true, PL_WARN>() << msg << endl;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "ehfun failed: " << e.what() << endl;
    }
  }

  int IdasInterface::jtimes(double t, N_Vector xz, N_Vector xzdot, N_Vector rr, N_Vector v,
                                   N_Vector Jv, double cj, void *user_data,
                                   N_Vector tmp1, N_Vector tmp2) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = NV_DATA_S(v);
      m->arg[5] = NV_DATA_S(v)+s.nx_;
      m->res[0] = NV_DATA_S(Jv);
      m->res[1] = NV_DATA_S(Jv)+s.nx_;
      s.calc_function(m, "jtimesF");

      // Subtract state derivative to get residual
      casadi_axpy(s.nx_, -cj, NV_DATA_S(v), NV_DATA_S(Jv));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimes failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::jtimesB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector vB, N_Vector JvB,
                                    double cjB, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = NV_DATA_S(xzB);
      m->arg[5] = NV_DATA_S(xzB)+s.nrx_;
      m->arg[6] = m->rp;
      m->arg[7] = NV_DATA_S(vB);
      m->arg[8] = NV_DATA_S(vB)+s.nrx_;
      m->res[0] = NV_DATA_S(JvB);
      m->res[1] = NV_DATA_S(JvB) + s.nrx_;
      s.calc_function(m, "jtimesB");

      // Subtract state derivative to get residual
      casadi_axpy(s.nrx_, cjB, NV_DATA_S(vB), NV_DATA_S(JvB));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimesB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::resS(int Ns, double t, N_Vector xz, N_Vector xzdot, N_Vector resval,
                                 N_Vector *xzF, N_Vector *xzdotF, N_Vector *rrF, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;

      // Commented out since a new implementation currently cannot be tested
      casadi_error("Commented out, #884, #794.");

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "resS failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::init_memory(void* mem) const {
    SundialsInterface::init_memory(mem);
    auto m = to_mem(mem);

    // Sundials return flag
    int flag;

    // Create IDAS memory block
    m->mem = IDACreate();
    if (m->mem==0) throw CasadiException("IDACreate(): Creation failed");

    // Allocate n-vectors for ivp
    m->xzdot = N_VNew_Serial(nx_+nz_);

    // Initialize Idas
    double t0 = 0;
    N_VConst(0.0, m->xz);
    N_VConst(0.0, m->xzdot);
    IDAInit(m->mem, res, t0, m->xz, m->xzdot);
    log("IdasInterface::init", "IDA initialized");

    // Set error handler function
    flag = IDASetErrHandlerFn(m->mem, ehfun, &m);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetErrHandlerFn");

    // Include algebraic variables in error testing
    flag = IDASetSuppressAlg(m->mem, suppress_algebraic_);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetSuppressAlg");

    // Maxinum order for the multistep method
    flag = IDASetMaxOrd(m->mem, max_multistep_order_);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetMaxOrd");

    // Set user data
    flag = IDASetUserData(m->mem, m);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetUserData");

    // Set maximum step size
    flag = IDASetMaxStep(m->mem, max_step_size_);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetMaxStep");

    if (!abstolv_.empty()) {
      // Vector absolute tolerances
      N_Vector nv_abstol = N_VNew_Serial(abstolv_.size());
      copy(abstolv_.begin(), abstolv_.end(), NV_DATA_S(nv_abstol));
      flag = IDASVtolerances(m->mem, reltol_, nv_abstol);
      casadi_assert_message(flag == IDA_SUCCESS, "IDASVtolerances");
      N_VDestroy_Serial(nv_abstol);
    } else {
      // Scalar absolute tolerances
      flag = IDASStolerances(m->mem, reltol_, abstol_);
      casadi_assert_message(flag == IDA_SUCCESS, "IDASStolerances");
    }

    // Maximum number of steps
    IDASetMaxNumSteps(m->mem, max_num_steps_);
    if (flag != IDA_SUCCESS) idas_error("IDASetMaxNumSteps", flag);

    // Set algebraic components
    N_Vector id = N_VNew_Serial(nx_+nz_);
    fill_n(NV_DATA_S(id), nx_, 1);
    fill_n(NV_DATA_S(id)+nx_, nz_, 0);

    // Pass this information to IDAS
    flag = IDASetId(m->mem, id);
    if (flag != IDA_SUCCESS) idas_error("IDASetId", flag);

    // Delete the allocated memory
    N_VDestroy_Serial(id);

    // attach a linear solver
    switch (linsol_f_) {
    case SD_DENSE:
      initDenseLinsol(m);
      break;
    case SD_BANDED:
      initBandedLinsol(m);
      break;
    case SD_ITERATIVE:
      initIterativeLinsol(m);
      break;
    case SD_USER_DEFINED:
      initUserDefinedLinsol(m);
      break;
    default: casadi_error("Uncaught switch");
    }

    // Quadrature equations
    if (nq_>0) {

      // Initialize quadratures in IDAS
      flag = IDAQuadInit(m->mem, rhsQ, m->q);
      if (flag != IDA_SUCCESS) idas_error("IDAQuadInit", flag);

      // Should the quadrature errors be used for step size control?
      if (quad_err_con_) {
        flag = IDASetQuadErrCon(m->mem, true);
        casadi_assert_message(flag == IDA_SUCCESS, "IDASetQuadErrCon");

        // Quadrature error tolerances
        // TODO(Joel): vector absolute tolerances
        flag = IDAQuadSStolerances(m->mem, reltol_, abstol_);
        if (flag != IDA_SUCCESS) idas_error("IDAQuadSStolerances", flag);
      }
    }

    log("IdasInterface::init", "attached linear solver");

    // Adjoint sensitivity problem
    if (nrx_>0) {
      m->rxzdot = N_VNew_Serial(nrx_+nrz_);
      N_VConst(0.0, m->rxz);
      N_VConst(0.0, m->rxzdot);
    }
    log("IdasInterface::init", "initialized adjoint sensitivities");

    m->isInitTaping = false;
    m->isInitAdj = false;
  }

  void IdasInterface::reset(IntegratorMemory* mem, double t, const double* _x,
                            const double* _z, const double* _p) const {
    log("IdasInterface::reset", "begin");
    auto m = to_mem(mem);

    // Reset the base classes
    SundialsInterface::reset(mem, t, _x, _z, _p);

    if (nrx_>0 && !m->isInitTaping)
      initTaping(m);

    // Return flag
    int flag;

    // Re-initialize
    copy(init_xdot_.begin(), init_xdot_.end(), NV_DATA_S(m->xzdot));
    flag = IDAReInit(m->mem, grid_.front(), m->xz, m->xzdot);
    if (flag != IDA_SUCCESS) idas_error("IDAReInit", flag);
    log("IdasInterface::reset", "re-initialized IVP solution");


    // Re-initialize quadratures
    if (nq_>0) {
      flag = IDAQuadReInit(m->mem, m->q);
      if (flag != IDA_SUCCESS) idas_error("IDAQuadReInit", flag);
      log("IdasInterface::reset", "re-initialized quadratures");
    }

    // Turn off sensitivities
    flag = IDASensToggleOff(m->mem);
    if (flag != IDA_SUCCESS) idas_error("IDASensToggleOff", flag);

    // Correct initial conditions, if necessary
    if (calc_ic_) {
      correctInitialConditions(m);
    }

    // Re-initialize backward integration
    if (nrx_>0) {
      flag = IDAAdjReInit(m->mem);
      if (flag != IDA_SUCCESS) idas_error("IDAAdjReInit", flag);
    }

    // Set the stop time of the integration -- don't integrate past this point
    if (stop_at_end_) setStopTime(m, grid_.back());

    log("IdasInterface::reset", "end");
  }


  void IdasInterface::correctInitialConditions(IdasMemory* m) const {
    log("IdasInterface::correctInitialConditions", "begin");

    // Calculate consistent initial conditions
    int flag = IDACalcIC(m->mem, IDA_YA_YDP_INIT , first_time_);
    if (flag != IDA_SUCCESS) idas_error("IDACalcIC", flag);

    // Retrieve the initial values
    flag = IDAGetConsistentIC(m->mem, m->xz, m->xzdot);
    if (flag != IDA_SUCCESS) idas_error("IDAGetConsistentIC", flag);

    // Print progress
    log("IdasInterface::correctInitialConditions", "end");
  }

  void IdasInterface::
  advance(IntegratorMemory* mem, double t, double* x, double* z, double* q) const {
    auto m = to_mem(mem);

    casadi_assert_message(t>=grid_.front(), "IdasInterface::integrate(" << t << "): "
                          "Cannot integrate to a time earlier than t0 (" << grid_.front() << ")");
    casadi_assert_message(t<=grid_.back() || !stop_at_end_, "IdasInterface::integrate("
                          << t << "): "
                          "Cannot integrate past a time later than tf (" << grid_.back() << ") "
                          "unless stop_at_end is set to False.");

    // Integrate, unless already at desired time
    double ttol = 1e-9;   // tolerance
    if (fabs(m->t-t)>=ttol) {
      // Integrate forward ...
      if (nrx_>0) {
        // ... with taping
        int flag = IDASolveF(m->mem, t, &m->t, m->xz, m->xzdot, IDA_NORMAL, &m->ncheck);
        if (flag != IDA_SUCCESS && flag != IDA_TSTOP_RETURN) idas_error("IDASolveF", flag);
      } else {
        // ... without taping
        int flag = IDASolve(m->mem, t, &m->t, m->xz, m->xzdot, IDA_NORMAL);
        if (flag != IDA_SUCCESS && flag != IDA_TSTOP_RETURN) idas_error("IDASolve", flag);
      }

      // Get quadratures
      if (nq_>0) {
        double tret;
        int flag = IDAGetQuad(m->mem, &tret, m->q);
        if (flag != IDA_SUCCESS) idas_error("IDAGetQuad", flag);
      }
    }

    // Set function outputs
    casadi_copy(NV_DATA_S(m->xz), nx_, x);
    casadi_copy(NV_DATA_S(m->xz)+nx_, nz_, z);
    casadi_copy(NV_DATA_S(m->q), nq_, q);

    // Print statistics
    if (print_stats_) printStats(m, userOut());

    int flag = IDAGetIntegratorStats(m->mem, &m->nsteps, &m->nfevals, &m->nlinsetups,
                                     &m->netfails, &m->qlast, &m->qcur, &m->hinused,
                                     &m->hlast, &m->hcur, &m->tcur);
    if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStats", flag);
  }

  void IdasInterface::resetB(IntegratorMemory* mem, double t, const double* rx,
                             const double* rz, const double* rp) const {
    log("IdasInterface::resetB", "begin");
    auto m = to_mem(mem);
    int flag;

    // Reset the base classes
    SundialsInterface::resetB(mem, t, rx, rz, rp);

    if (!m->isInitAdj) { // First call
      // Create backward problem
      flag = IDACreateB(m->mem, &m->whichB);
      if (flag != IDA_SUCCESS) idas_error("IDACreateB", flag);

      // Initialize the backward problem
      double tB0 = grid_.back();
      flag = IDAInitB(m->mem, m->whichB, resB, tB0, m->rxz, m->rxzdot);
      if (flag != IDA_SUCCESS) idas_error("IDAInitB", flag);

      // Set tolerances
      flag = IDASStolerancesB(m->mem, m->whichB, reltolB_, abstolB_);
      if (flag!=IDA_SUCCESS) idas_error("IDASStolerancesB", flag);

      // User data
      flag = IDASetUserDataB(m->mem, m->whichB, m);
      if (flag != IDA_SUCCESS) idas_error("IDASetUserDataB", flag);

      // Maximum number of steps
      IDASetMaxNumStepsB(m->mem, m->whichB, max_num_steps_);
      if (flag != IDA_SUCCESS) idas_error("IDASetMaxNumStepsB", flag);

      // Set algebraic components
      N_Vector id = N_VNew_Serial(nrx_+nrz_);
      fill_n(NV_DATA_S(id), nrx_, 1);
      fill_n(NV_DATA_S(id)+nrx_, nrz_, 0);

      // Pass this information to IDAS
      flag = IDASetIdB(m->mem, m->whichB, id);
      if (flag != IDA_SUCCESS) idas_error("IDASetIdB", flag);

      // Delete the allocated memory
      N_VDestroy_Serial(id);

      // attach linear solver
      switch (linsol_g_) {
      case SD_DENSE: initDenseLinsolB(m); break;
      case SD_BANDED: initBandedLinsolB(m); break;
      case SD_ITERATIVE: initIterativeLinsolB(m); break;
      case SD_USER_DEFINED: initUserDefinedLinsolB(m); break;
      default: casadi_error("Uncaught switch");
      }

      // Quadratures for the adjoint problem
      flag = IDAQuadInitB(m->mem, m->whichB, rhsQB, m->rq);
      if (flag!=IDA_SUCCESS) idas_error("IDAQuadInitB", flag);

      // Quadrature error control
      if (quad_err_con_) {
        flag = IDASetQuadErrConB(m->mem, m->whichB, true);
        if (flag != IDA_SUCCESS) idas_error("IDASetQuadErrConB", flag);

        flag = IDAQuadSStolerancesB(m->mem, m->whichB, reltolB_, abstolB_);
        if (flag != IDA_SUCCESS) idas_error("IDAQuadSStolerancesB", flag);
      }

      // Mark initialized
      m->isInitAdj = true;
    } else { // Re-initialize
      flag = IDAReInitB(m->mem, m->whichB, grid_.back(), m->rxz, m->rxzdot);
      if (flag != IDA_SUCCESS) idas_error("IDAReInitB", flag);

      if (nrq_>0) {
        flag = IDAQuadReInit(IDAGetAdjIDABmem(m->mem, m->whichB), m->rq);
        // flag = IDAQuadReInitB(m->mem, m->whichB[dir], m->rq[dir]); // BUG in Sundials
        //                                                      // do not use this!
        if (flag!=IDA_SUCCESS) idas_error("IDAQuadReInitB", flag);
      }
    }

    // Correct initial values for the integration if necessary
    if (calc_icB_) {
      log("IdasInterface::resetB", "IDACalcICB begin");
      flag = IDACalcICB(m->mem, m->whichB, grid_.front(), m->xz, m->xzdot);
      if (flag != IDA_SUCCESS) idas_error("IDACalcICB", flag);
      log("IdasInterface::resetB", "IDACalcICB end");

      // Retrieve the initial values
      flag = IDAGetConsistentICB(m->mem, m->whichB, m->rxz, m->rxzdot);
      if (flag != IDA_SUCCESS) idas_error("IDAGetConsistentICB", flag);

    }

    log("IdasInterface::resetB", "end");

  }

  void IdasInterface::retreat(IntegratorMemory* mem, double t, double* rx,
                              double* rz, double* rq) const {
    auto m = to_mem(mem);

    // Integrate, unless already at desired time
    if (t<m->t) {
      int flag = IDASolveB(m->mem, t, IDA_NORMAL);
      if (flag<IDA_SUCCESS) idas_error("IDASolveB", flag);

      // Get backward state
      flag = IDAGetB(m->mem, m->whichB, &m->t, m->rxz, m->rxzdot);
      if (flag!=IDA_SUCCESS) idas_error("IDAGetB", flag);

      // Get backward qudratures
      if (nrq_>0) {
        flag = IDAGetQuadB(m->mem, m->whichB, &m->t, m->rq);
        if (flag!=IDA_SUCCESS) idas_error("IDAGetQuadB", flag);
      }
    }

    // Save outputs
    casadi_copy(NV_DATA_S(m->rxz), nrx_, rx);
    casadi_copy(NV_DATA_S(m->rxz)+nrx_, nrz_, rz);
    casadi_copy(NV_DATA_S(m->rq), nrq_, rq);

    IDAMem IDA_mem = IDAMem(m->mem);
    IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
    IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;
    int flag = IDAGetIntegratorStats(IDAB_mem->IDA_mem, &m->nstepsB, &m->nfevalsB,
                                     &m->nlinsetupsB,
                                     &m->netfailsB, &m->qlastB, &m->qcurB, &m->hinusedB,
                                     &m->hlastB,
                                     &m->hcurB, &m->tcurB);
    if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStatsB", flag);
  }

  void IdasInterface::printStats(IntegratorMemory* mem, std::ostream &stream) const {
    auto m = to_mem(mem);

    long nsteps, nfevals, nlinsetups, netfails;
    int qlast, qcur;
    double hinused, hlast, hcur, tcur;
    int flag = IDAGetIntegratorStats(m->mem, &nsteps, &nfevals, &nlinsetups, &netfails, &qlast,
                                     &qcur, &hinused, &hlast, &hcur, &tcur);
    if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStats", flag);

    // Get the number of right hand side evaluations in the linear solver
    long nfevals_linsol=0;
    switch (linsol_f_) {
    case SD_DENSE:
    case SD_BANDED:
      flag = IDADlsGetNumResEvals(m->mem, &nfevals_linsol);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsGetNumResEvals", flag);
      break;
    case SD_ITERATIVE:
      flag = IDASpilsGetNumResEvals(m->mem, &nfevals_linsol);
      if (flag!=IDA_SUCCESS) idas_error("IDASpilsGetNumResEvals", flag);
      break;
    default:
      nfevals_linsol = 0;
    }

    stream << "number of steps taken by IDAS:            " << nsteps << std::endl;
    stream << "number of calls to the user's f function: " << (nfevals + nfevals_linsol)
           << std::endl;
    stream << "   step calculation:                      " << nfevals << std::endl;
    stream << "   linear solver:                         " << nfevals_linsol << std::endl;
    stream << "number of calls made to the linear solver setup function: " << nlinsetups
           << std::endl;
    stream << "number of error test failures: " << netfails << std::endl;
    stream << "method order used on the last internal step: " << qlast << std::endl;
    stream << "method order to be used on the next internal step: " << qcur << std::endl;
    stream << "actual value of initial step size: " << hinused << std::endl;
    stream << "step size taken on the last internal step: " << hlast << std::endl;
    stream << "step size to be attempted on the next internal step: " << hcur << std::endl;
    stream << "current internal time reached: " << tcur << std::endl;
    stream << std::endl;

    stream << "number of checkpoints stored: " << m->ncheck << endl;
    stream << std::endl;
  }

  void IdasInterface::idas_error(const string& module, int flag) {
    // Find the error
    char* flagname = IDAGetReturnFlagName(flag);
    stringstream ss;
    ss << "Module \"" << module << "\" returned flag " << flag << " (\"" << flagname << "\").";
    ss << " Consult Idas documentation." << std::endl;
    free(flagname);

    // Heuristics
    if (
        (module=="IDACalcIC" && (flag==IDA_CONV_FAIL || flag==IDA_NO_RECOVERY ||
                                 flag==IDA_LINESEARCH_FAIL)) ||
        (module=="IDASolve" && flag ==IDA_ERR_FAIL)
        ) {
      ss << "Some common causes for this error: " << std::endl;
      ss << "  - providing an initial guess for which 0=g(y, z, t) is not invertible wrt y. "
         << std::endl;
      ss << "  - having a DAE-index higher than 1 such that 0=g(y, z, t) is not invertible wrt y "
          "over the whole domain." << std::endl;
      ss << "  - having set abstol or reltol too small." << std::endl;
      ss << "  - using 'calcic'=True for systems that are not semi-explicit index-one. "
          "You must provide consistent initial conditions yourself in this case. " << std::endl;
      ss << "  - your problem is too hard for IDAcalcIC to solve. Provide consistent "
          "initial conditions yourself." << std::endl;
    }

    casadi_error(ss.str());
  }

  int IdasInterface::rhsQ(double t, N_Vector xz, N_Vector xzdot, N_Vector rhsQ,
                                 void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(xz);
      m->arg[1] = NV_DATA_S(xz)+s.nx_;
      m->arg[2] = m->p;
      m->arg[3] = &t;
      m->res[0] = NV_DATA_S(rhsQ);
      s.calc_function(m, "quadF");

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQ failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::rhsQS(int Ns, double t, N_Vector xz, N_Vector xzdot, N_Vector *xzF,
                                  N_Vector *xzdotF, N_Vector rrQ, N_Vector *qdotF, void *user_data,
                                  N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {

    try {
      auto m = to_mem(user_data);
      auto& s = m->self;

      // Commented out since a new implementation currently cannot be tested
      casadi_error("Commented out, #884, #794.");

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQS failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::resB(double t, N_Vector xz, N_Vector xzdot, N_Vector rxz,
                                 N_Vector rxzdot, N_Vector rr, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(rxz);
      m->arg[1] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[2] = m->rp;
      m->arg[3] = NV_DATA_S(xz);
      m->arg[4] = NV_DATA_S(xz)+s.nx_;
      m->arg[5] = m->p;
      m->arg[6] = &t;
      m->res[0] = NV_DATA_S(rr);
      m->res[1] = NV_DATA_S(rr)+s.nrx_;
      s.calc_function(m, "daeB");

      // Subtract state derivative to get residual
      casadi_axpy(s.nrx_, 1., NV_DATA_S(rxzdot), NV_DATA_S(rr));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "resB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::rhsQB(double t, N_Vector xz, N_Vector xzdot, N_Vector rxz,
                                  N_Vector rxzdot, N_Vector rqdot, void *user_data) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = NV_DATA_S(rxz);
      m->arg[1] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[2] = m->rp;
      m->arg[3] = NV_DATA_S(xz);
      m->arg[4] = NV_DATA_S(xz)+s.nx_;
      m->arg[5] = m->p;
      m->arg[6] = &t;
      m->res[0] = NV_DATA_S(rqdot);
      s.calc_function(m, "quadB");

      // Negate (note definition of g)
      casadi_scal(s.nrq_, -1., NV_DATA_S(rqdot));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::djac(long Neq, double t, double cj, N_Vector xz, N_Vector xzdot,
                                 N_Vector rr, DlsMat Jac, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = &cj;
      m->res[0] = m->jac;
      s.calc_function(m, "jacF");

      // Save to Jac
      const Sparsity& sp = s.get_function("jacF").sparsity_out(0);
      const int* colind = sp.colind();
      int ncol = sp.size2();
      const int* row = sp.row();
      for (int cc=0; cc<ncol; ++cc) {
        for (int el=colind[cc]; el<colind[cc+1]; ++el) {
          DENSE_ELEM(Jac, row[el], cc) = m->jac[el];
        }
      }

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "djac failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::djacB(long int NeqB, double t, double cj, N_Vector xz, N_Vector xzdot,
                                  N_Vector rxz, N_Vector rxzdot, N_Vector rrr, DlsMat JacB,
                                  void *user_data,
                                  N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(rxz);
      m->arg[2] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[3] = m->rp;
      m->arg[4] = NV_DATA_S(xz);
      m->arg[5] = NV_DATA_S(xz)+s.nx_;
      m->arg[6] = m->p;
      m->arg[7] = &cj;
      m->res[0] = m->jacB;
      s.calc_function(m, "jacB");

      // Save to JacB
      const Sparsity& sp = s.get_function("jacB").sparsity_out(0);
      const int* colind = sp.colind();
      int ncol = sp.size2();
      const int* row = sp.row();
      for (int cc=0; cc<ncol; ++cc) {
        for (int el=colind[cc]; el<colind[cc+1]; ++el) {
          DENSE_ELEM(JacB, row[el], cc) = m->jacB[el];
        }
      }
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "djacB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::bjac(long Neq, long mupper, long mlower, double t, double cj,
                                 N_Vector xz, N_Vector xzdot, N_Vector rr,
                                 DlsMat Jac, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = &cj;
      m->res[0] = m->jac;
      s.calc_function(m, "jacF");

      // Save to Jac
      const Sparsity& sp = s.get_function("jacF").sparsity_out(0);
      const int* colind = sp.colind();
      int ncol = sp.size2();
      const int* row = sp.row();
      for (int cc=0; cc<ncol; ++cc) {
        for (int el=colind[cc]; el<colind[cc+1]; ++el) {
          int rr = row[el];
          if (cc-rr<=mupper && rr-cc<=mlower)
            BAND_ELEM(Jac, rr, cc) = m->jac[el];
        }
      }
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "bjac failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::
  bjacB(long NeqB, long mupperB, long mlowerB, double t, double cj,
                N_Vector xz, N_Vector xzdot, N_Vector rxz, N_Vector rxzdot,
                N_Vector resval, DlsMat JacB, void *user_data, N_Vector tmp1B,
                N_Vector tmp2B, N_Vector tmp3B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(rxz);
      m->arg[2] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[3] = m->rp;
      m->arg[4] = NV_DATA_S(xz);
      m->arg[5] = NV_DATA_S(xz)+s.nx_;
      m->arg[6] = m->p;
      m->arg[7] = &cj;
      m->res[0] = m->jacB;
      s.calc_function(m, "jacB");

      // Save to JacB
      const Sparsity& sp = s.get_function("jacB").sparsity_out(0);
      const int* colind = sp.colind();
      int ncol = sp.size2();
      const int* row = sp.row();
      for (int cc=0; cc<ncol; ++cc) {
        for (int el=colind[cc]; el<colind[cc+1]; ++el) {
          int rr = row[el];
          if (cc-rr<=mupperB && rr-cc<=mlowerB)
            BAND_ELEM(JacB, rr, cc) = m->jacB[el];
        }
      }
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "bjacB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::setStopTime(IntegratorMemory* mem, double tf) const {
    // Set the stop time of the integration -- don't integrate past this point
    auto m = to_mem(mem);
    auto& s = m->self;
    int flag = IDASetStopTime(m->mem, tf);
    if (flag != IDA_SUCCESS) idas_error("IDASetStopTime", flag);
  }

  int IdasInterface::psolve(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                    N_Vector rvec, N_Vector zvec, double cj, double delta,
                                    void *user_data, N_Vector tmp) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      // Copy input to output, if necessary
      if (rvec!=zvec) {
        N_VScale(1.0, rvec, zvec);
      }

      // Solve the (possibly factorized) system
      const Function& linsol = s.get_function("linsolF");
      casadi_assert_message(linsol.nnz_out(0) == NV_LENGTH_S(zvec), "Assertion error: "
                            << linsol.nnz_out(0) << " == " << NV_LENGTH_S(zvec));
      linsol.linsol_solve(NV_DATA_S(zvec));
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolve failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psolveB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector rvecB,
                                    N_Vector zvecB, double cjB, double deltaB,
                                    void *user_data, N_Vector tmpB) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      // Copy input to output, if necessary
      if (rvecB!=zvecB) {
        N_VScale(1.0, rvecB, zvecB);
      }

      const Function& linsolB = s.get_function("linsolB");
      casadi_assert(!linsolB.is_null());
      casadi_assert_message(linsolB.nnz_out(0) == NV_LENGTH_S(zvecB),
                            "Assertion error: " << linsolB.nnz_out(0)
                            << " == " << NV_LENGTH_S(zvecB));
      linsolB.linsol_solve(NV_DATA_S(zvecB));

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolveB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetup(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                   double cj, void* user_data,
                                   N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(xz);
      m->arg[2] = NV_DATA_S(xz)+s.nx_;
      m->arg[3] = m->p;
      m->arg[4] = &cj;
      m->res[0] = m->jac;
      s.calc_function(m, "jacF");

      // Prepare the solution of the linear system (e.g. factorize)
      const Function& linsol = s.get_function("linsolF");
      linsol.setup(m->arg+LINSOL_NUM_IN, m->res+LINSOL_NUM_OUT, m->iw, m->w);
      linsol.linsol_factorize(m->jac);

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetup failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetupB(double t, N_Vector xz, N_Vector xzdot,
                                    N_Vector rxz, N_Vector rxzdot,
                                    N_Vector rresval, double cj, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    try {
      auto m = to_mem(user_data);
      auto& s = m->self;
      m->arg[0] = &t;
      m->arg[1] = NV_DATA_S(rxz);
      m->arg[2] = NV_DATA_S(rxz)+s.nrx_;
      m->arg[3] = m->rp;
      m->arg[4] = NV_DATA_S(xz);
      m->arg[5] = NV_DATA_S(xz)+s.nx_;
      m->arg[6] = m->p;
      m->arg[7] = &cj;
      m->res[0] = m->jacB;
      s.calc_function(m, "jacB");

      // Prepare the solution of the linear system (e.g. factorize)
      const Function& linsolB = s.get_function("linsolB");
      linsolB.setup(m->arg+LINSOL_NUM_IN, m->res+LINSOL_NUM_OUT, m->iw, m->w);
      linsolB.linsol_factorize(m->jacB);

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetupB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::lsetup(IDAMem IDA_mem, N_Vector xz, N_Vector xzdot, N_Vector resp,
                                    N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3) {
    // Current time
    double t = IDA_mem->ida_tn;

    // Multiple of df_dydot to be added to the matrix
    double cj = IDA_mem->ida_cj;

    // Call the preconditioner setup function (which sets up the linear solver)
    if (psetup(t, xz, xzdot, 0, cj, IDA_mem->ida_lmem,
      vtemp1, vtemp1, vtemp3)) return 1;

    return 0;
  }

  int IdasInterface::lsetupB(IDAMem IDA_mem, N_Vector xzB, N_Vector xzdotB, N_Vector respB,
                                     N_Vector vtemp1B, N_Vector vtemp2B, N_Vector vtemp3B) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      auto& s = m->self;
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;
      int flag;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;

      IDA_mem = static_cast<IDAMem>(IDA_mem->ida_user_data);

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }
      // Call the preconditioner setup function (which sets up the linear solver)
      if (psetupB(t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
        xzB, xzdotB, 0, cj, static_cast<void*>(m), vtemp1B, vtemp1B, vtemp3B)) return 1;

      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsetupB failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsolve(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xz,
                                   N_Vector xzdot, N_Vector rr) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      auto& s = m->self;

      // Current time
      double t = IDA_mem->ida_tn;

      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;

      // Accuracy
      double delta = 0.0;

      // Call the preconditioner solve function (which solves the linear system)
      if (psolve(t, xz, xzdot, rr, b, b, cj,
        delta, static_cast<void*>(m), 0)) return 1;

      // Scale the correction to account for change in cj
      if (s.cj_scaling_) {
        double cjratio = IDA_mem->ida_cjratio;
        if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
      }

      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolve failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsolveB(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector rrB) {
    try {
      auto m = to_mem(IDA_mem->ida_lmem);
      auto& s = m->self;
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;
      int flag;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;
      double cjratio = IDA_mem->ida_cjratio;

      IDA_mem = (IDAMem) IDA_mem->ida_user_data;

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }

      // Accuracy
      double delta = 0.0;

      // Call the preconditioner solve function (which solves the linear system)
      if (psolveB(t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp, xzB, xzdotB,
        rrB, b, b, cj, delta, static_cast<void*>(m), 0)) return 1;

      // Scale the correction to account for change in cj
      if (s.cj_scaling_) {
        if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
      }
      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolveB failed: " << e.what() << endl;
      return -1;
    }
  }

  void IdasInterface::initDenseLinsol(IdasMemory* m) const {
    // Dense jacobian
    int flag = IDADense(m->mem, nx_+nz_);
    if (flag != IDA_SUCCESS) idas_error("IDADense", flag);
    if (exact_jacobian_) {
      flag = IDADlsSetDenseJacFn(m->mem, djac);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetDenseJacFn", flag);
    }
  }

  void IdasInterface::initBandedLinsol(IdasMemory* m) const {
    // Banded jacobian
    pair<int, int> bw = getBandwidth();
    int flag = IDABand(m->mem, nx_+nz_, bw.first, bw.second);
    if (flag != IDA_SUCCESS) idas_error("IDABand", flag);

    // Banded Jacobian information
    if (exact_jacobian_) {
      flag = IDADlsSetBandJacFn(m->mem, bjac);
      if (flag != IDA_SUCCESS) idas_error("IDADlsSetBandJacFn", flag);
    }
  }

  void IdasInterface::initIterativeLinsol(IdasMemory* m) const {
    // Attach an iterative solver
    int flag;
    switch (itsol_f_) {
    case SD_GMRES:
      flag = IDASpgmr(m->mem, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASpgmr", flag);
      break;
    case SD_BCGSTAB:
      flag = IDASpbcg(m->mem, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASpbcg", flag);
      break;
    case SD_TFQMR:
      flag = IDASptfqmr(m->mem, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASptfqmr", flag);
      break;
    default: casadi_error("Uncaught switch");
    }

    // Attach functions for jacobian information
    if (exact_jacobian_) {
      flag = IDASpilsSetJacTimesVecFn(m->mem, jtimes);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetJacTimesVecFn", flag);
    }

    // Add a preconditioner
    if (use_preconditioner_) {
      casadi_assert_message(has_function("jacF"), "No Jacobian function");
      casadi_assert_message(has_function("linsolF"), "No linear solver");
      flag = IDASpilsSetPreconditioner(m->mem, psetup, psolve);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetPreconditioner", flag);
    }
  }

  void IdasInterface::initUserDefinedLinsol(IdasMemory* m) const {
    casadi_assert_message(has_function("jacF"), "No Jacobian function");
    casadi_assert_message(has_function("linsolF"), "No linear solver");
    IDAMem IDA_mem = IDAMem(m->mem);
    IDA_mem->ida_lmem   = m;
    IDA_mem->ida_lsetup = lsetup;
    IDA_mem->ida_lsolve = lsolve;
    IDA_mem->ida_setupNonNull = TRUE;
  }

  void IdasInterface::initDenseLinsolB(IdasMemory* m) const {
    // Dense jacobian
    int flag = IDADenseB(m->mem, m->whichB, nrx_+nrz_);
    if (flag != IDA_SUCCESS) idas_error("IDADenseB", flag);
    if (exact_jacobianB_) {
      // Pass to IDA
      flag = IDADlsSetDenseJacFnB(m->mem, m->whichB, djacB);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetDenseJacFnB", flag);
    }
  }

  void IdasInterface::initBandedLinsolB(IdasMemory* m) const {
    pair<int, int> bw = getBandwidthB();
    int flag = IDABandB(m->mem, m->whichB, nrx_+nrz_, bw.first, bw.second);
    if (flag != IDA_SUCCESS) idas_error("IDABand", flag);
    if (exact_jacobianB_) {
      // Pass to IDA
      flag = IDADlsSetBandJacFnB(m->mem, m->whichB, bjacB);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetBandJacFnB", flag);
    }
  }

  void IdasInterface::initIterativeLinsolB(IdasMemory* m) const {
    int flag;
    switch (itsol_g_) {
    case SD_GMRES:
      flag = IDASpgmrB(m->mem, m->whichB, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASpgmrB", flag);
      break;
    case SD_BCGSTAB:
      flag = IDASpbcgB(m->mem, m->whichB, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASpbcgB", flag);
      break;
    case SD_TFQMR:
      flag = IDASptfqmrB(m->mem, m->whichB, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASptfqmrB", flag);
      break;
    default: casadi_error("Uncaught switch");
    }

    // Attach functions for jacobian information
    if (exact_jacobianB_) {
#ifdef WITH_SYSTEM_SUNDIALS
      flag = IDASpilsSetJacTimesVecFnBPatched(m->mem, m->whichB, jtimesB);
#else
      flag = IDASpilsSetJacTimesVecFnB(m->mem, m->whichB, jtimesB);
#endif
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetJacTimesVecFnB", flag);
    }

    // Add a preconditioner
    if (use_preconditionerB_) {
      casadi_assert_message(has_function("jacB"), "No Jacobian function");
      casadi_assert_message(has_function("linsolB"), "No linear solver");
      flag = IDASpilsSetPreconditionerB(m->mem, m->whichB, psetupB, psolveB);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetPreconditionerB", flag);
    }

  }

  void IdasInterface::initUserDefinedLinsolB(IdasMemory* m) const {
    casadi_assert_message(has_function("jacB"), "No Jacobian function");
    casadi_assert_message(has_function("linsolB"), "No linear solver");

    //  Set fields in the IDA memory
    IDAMem IDA_mem = IDAMem(m->mem);
    IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
    IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;
    IDAB_mem->ida_lmem   = m;

    IDAB_mem->IDA_mem->ida_lmem = m;
    IDAB_mem->IDA_mem->ida_lsetup = lsetupB;
    IDAB_mem->IDA_mem->ida_lsolve = lsolveB;
    IDAB_mem->IDA_mem->ida_setupNonNull = TRUE;
  }

  template<typename MatType>
  Function IdasInterface::getJacF() {
    vector<MatType> a = MatType::get_input(oracle_);
    vector<MatType> r = oracle_(a);

    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jacobian(r[DE_ODE], a[DE_X]) - cj*MatType::eye(nx_);
    if (nz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jacobian(r[DE_ALG], a[DE_X])),
                    vertcat(MatType::jacobian(r[DE_ODE], a[DE_Z]),
                            MatType::jacobian(r[DE_ALG], a[DE_Z])));
    }

    return Function("jacF", {a[DE_T], a[DE_X], a[DE_Z], a[DE_P], cj},
                    {jac});
  }

  template<typename MatType>
  Function IdasInterface::getJacB() {
    vector<MatType> a = MatType::get_input(oracle_);
    vector<MatType> r = oracle_(a);

    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jacobian(r[DE_RODE], a[DE_RX]) + cj*MatType::eye(nrx_);
    if (nrz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jacobian(r[DE_RALG], a[DE_RX])),
                    vertcat(MatType::jacobian(r[DE_RODE], a[DE_RZ]),
                            MatType::jacobian(r[DE_RALG], a[DE_RZ])));
    }

    return Function("jacB", {a[DE_T], a[DE_RX], a[DE_RZ], a[DE_RP],
                             a[DE_X], a[DE_Z], a[DE_P], cj},
                    {jac});
  }

  IdasMemory::IdasMemory(const IdasInterface& s) : self(s) {
    this->mem = 0;
    this->xzdot = 0;
    this->rxzdot = 0;
    this->isInitAdj = false;
    this->isInitTaping = false;

    // Reset checkpoints counter
    this->ncheck = 0;
  }

  IdasMemory::~IdasMemory() {
    if (this->mem) IDAFree(&this->mem);
    if (this->xzdot) N_VDestroy_Serial(this->xzdot);
    if (this->rxzdot) N_VDestroy_Serial(this->rxzdot);
  }

  Dict IdasInterface::get_stats(void* mem) const {
    Dict stats = SundialsInterface::get_stats(mem);
    return stats;
  }


} // namespace casadi
