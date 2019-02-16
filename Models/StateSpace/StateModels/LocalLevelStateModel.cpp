// Copyright 2018 Google LLC. All Rights Reserved.
/*
  Copyright (C) 2005-2011 Steven L. Scott

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#include "Models/StateSpace/StateModels/LocalLevelStateModel.hpp"
#include "Models/StateSpace/StateModels/StateModel.hpp"
#include "LinAlg/DiagonalMatrix.hpp"
#include "cpputil/math_utils.hpp"
#include "distributions.hpp"

namespace BOOM {

  namespace {
    using LLSM = LocalLevelStateModel;
    using SLLSM = SharedLocalLevelStateModel;
  }

  LLSM::LocalLevelStateModel(double sigma)
      : ZeroMeanGaussianModel(sigma),
        state_transition_matrix_(new IdentityMatrix(1)),
        state_variance_matrix_(new ConstantMatrixParamView(1, Sigsq_prm())),
        initial_state_mean_(1),
        initial_state_variance_(1) {}

  LLSM::LocalLevelStateModel(const LocalLevelStateModel &rhs)
      : Model(rhs),
        StateModel(rhs),
        ZeroMeanGaussianModel(rhs),
        state_transition_matrix_(new IdentityMatrix(1)),
        state_variance_matrix_(new ConstantMatrixParamView(1, Sigsq_prm())),
        initial_state_mean_(rhs.initial_state_mean_),
        initial_state_variance_(rhs.initial_state_variance_) {}

  LocalLevelStateModel *LLSM::clone() const {
    return new LocalLevelStateModel(*this);
  }

  void LLSM::observe_state(const ConstVectorView &then,
                           const ConstVectorView &now, int time_now) {
    double current_level = now[0];
    double previous_level = then[0];
    double diff = current_level - previous_level;
    suf()->update_raw(diff);
  }

  uint LLSM::state_dimension() const { return 1; }

  void LLSM::simulate_state_error(RNG &rng, VectorView eta, int) const {
    eta[0] = rnorm_mt(rng, 0, sigma());
  }

  void LLSM::simulate_initial_state(RNG &rng, VectorView eta) const {
    eta[0] = rnorm_mt(rng, initial_state_mean_[0],
                      sqrt(initial_state_variance_(0, 0)));
  }

  Ptr<SparseMatrixBlock> LLSM::state_transition_matrix(int) const {
    return state_transition_matrix_;
  }

  Ptr<SparseMatrixBlock> LLSM::state_variance_matrix(int) const {
    return state_variance_matrix_;
  }

  Ptr<SparseMatrixBlock> LLSM::state_error_expander(int t) const {
    return state_transition_matrix(t);
  }

  Ptr<SparseMatrixBlock> LLSM::state_error_variance(int t) const {
    return state_variance_matrix(t);
  }

  SparseVector LLSM::observation_matrix(int) const {
    SparseVector ans(1);
    ans[0] = 1;
    return ans;
  }

  Vector LLSM::initial_state_mean() const { return initial_state_mean_; }

  SpdMatrix LLSM::initial_state_variance() const {
    return initial_state_variance_;
  }

  void LLSM::set_initial_state_mean(const Vector &m) {
    initial_state_mean_ = m;
  }

  void LLSM::set_initial_state_mean(double m) { initial_state_mean_[0] = m; }

  void LLSM::set_initial_state_variance(const SpdMatrix &v) {
    initial_state_variance_ = v;
  }

  void LLSM::set_initial_state_variance(double v) {
    initial_state_variance_(0, 0) = v;
  }

  void LLSM::update_complete_data_sufficient_statistics(
      int, const ConstVectorView &state_error_mean,
      const ConstSubMatrix &state_error_variance) {
    if (state_error_mean.size() != 1 || state_error_variance.nrow() != 1 ||
        state_error_variance.ncol() != 1) {
      report_error(
          "Wrong size arguments to LocalLevelStateModel::"
          "update_complete_data_sufficient_statistics.");
    }
    double mean = state_error_mean[0];
    double var = state_error_variance(0, 0);
    suf()->update_expected_value(1.0, mean, var + square(mean));
  }

  void LLSM::increment_expected_gradient(
      VectorView gradient, int t, const ConstVectorView &state_error_mean,
      const ConstSubMatrix &state_error_variance) {
    if (gradient.size() != 1 || state_error_mean.size() != 1 ||
        state_error_variance.nrow() != 1 || state_error_variance.ncol() != 1) {
      report_error(
          "Wrong size arguments to LocalLevelStateModel::"
          "increment_expected_gradient.");
    }
    double mean = state_error_mean[0];
    double var = state_error_variance(0, 0);
    double sigsq = ZeroMeanGaussianModel::sigsq();
    gradient[0] += (-.5 / sigsq) + .5 * (var + mean * mean) / (sigsq * sigsq);
  }

  //===========================================================================

  SLLSM::SharedLocalLevelStateModel(
      int number_of_factors, int ydim, MultivariateStateSpaceModelBase *host)
      : host_(host),
        coefficient_model_(new MultivariateRegressionModel(
            number_of_factors, ydim)),
        empty_(new EmptyMatrix),
        observation_coefficients_current_(false),
        initial_state_mean_(0),
        initial_state_variance_(0),
        initial_state_variance_cholesky_(0, 0)
  {
    for (int i = 0; i < number_of_factors; ++i) {
      innovation_models_.push_back(new ZeroMeanGaussianModel);
    }
    set_param_policy();
    initialize_model_matrices();
    set_observation_coefficients_observer();
  }

  SLLSM::SharedLocalLevelStateModel(const SLLSM &rhs) {
    operator=(rhs);
  }
  
  SLLSM & SLLSM::operator=(const SLLSM &rhs) {
    if (&rhs != this) {
      coefficient_model_ = rhs.coefficient_model_->clone();
      if (!empty_) empty_ = rhs.empty_->clone();
      initial_state_mean_ = rhs.initial_state_mean_;
      initial_state_variance_ = rhs.initial_state_variance_;
      initial_state_variance_cholesky_ = rhs.initial_state_variance_cholesky_;
      innovation_models_.clear();
      for (int i = 0; i < rhs.innovation_models_.size(); ++i) {
        innovation_models_.push_back(rhs.innovation_models_[i]->clone());
      }
      set_param_policy();
      initialize_model_matrices();
      set_observation_coefficients_observer();
    }
    return *this;
  }

  SLLSM::SharedLocalLevelStateModel(SLLSM &&rhs)
      : innovation_models_(std::move(rhs.innovation_models_)),
        coefficient_model_(std::move(rhs.coefficient_model_)),
        observation_coefficients_(std::move(rhs.observation_coefficients_)),
        empty_(std::move(rhs.empty_)),
        state_transition_matrix_(std::move(rhs.state_transition_matrix_)),
        state_variance_matrix_(std::move(rhs.state_variance_matrix_)),
        initial_state_mean_(std::move(rhs.initial_state_mean_)),
        initial_state_variance_(std::move(rhs.initial_state_variance_)),
        initial_state_variance_cholesky_(std::move(
            rhs.initial_state_variance_cholesky_))
  {
    set_param_policy();
    set_observation_coefficients_observer();
  }

  SLLSM & SLLSM::operator=(SLLSM &&rhs) {
    if (&rhs != this) {
      innovation_models_ = std::move(rhs.innovation_models_);
      coefficient_model_ = std::move(rhs.coefficient_model_);
      observation_coefficients_ = std::move(rhs.observation_coefficients_);
      state_transition_matrix_ = std::move(rhs.state_transition_matrix_);
      state_variance_matrix_ = std::move(rhs.state_variance_matrix_);
      initial_state_mean_ = std::move(rhs.initial_state_mean_);
      initial_state_variance_ = std::move(rhs.initial_state_variance_);
      initial_state_variance_cholesky_ = std::move(rhs.initial_state_variance_cholesky_);
    }
    set_observation_coefficients_observer();
    return *this;
  }

  SLLSM * SLLSM::clone() const {return new SLLSM(*this);}
  
  void SLLSM::clear_data() {
    for (int i = 0; i < innovation_models_.size(); ++i) {
      innovation_models_[i]->clear_data();
    }
    coefficient_model_->clear_data();
  }

  // Args:
  //   then: The portion of the state vector associated with this object at time
  //     point time_now - 1.
  //   now: The portion of the state vector associated with this object at time
  //     point time_now.
  //   time_now:  The index of the current time point.
  void SLLSM::observe_state(const ConstVectorView &then,
                            const ConstVectorView &now,
                            int time_now) {
    for (int i = 0; i < innovation_models_.size(); ++i) {
      double diff = now[i] - then[i];
      innovation_models_[i]->suf()->update_raw(diff);
    }
    // Residual y is the residual remaining after the other state components
    // have made their contributions.
    //
    // This function assumes that the state of the model has been set.
    const Selector &observed = host_->observed_status(time_now);
    if (observed.nvars() != observed.nvars_possible()) {
      std::ostringstream err;
      err << "The SharedLocalLevelStateModel assumes all observations are "
          << "fully observed.  ";
      // Once the MultivariateRegressionModel can handle partially observed data
      // then we can lift this restriction.  Not a huge priority though.
      report_error(err.str());
    }
    Vector residual_y =
        observed.select(host_->observation(time_now)) -
        (*host_->observation_coefficients(time_now, observed) 
         * host_->state(time_now))
        + observed.select(observation_coefficients_->matrix() * now);

    coefficient_model_->suf()->update_raw_data(residual_y, now, 1.0);
  }

  void SLLSM::simulate_state_error(RNG &rng, VectorView eta, int t) const {
    for (int i = 0; i < number_of_factors(); ++i) {
      eta[i] = rnorm_mt(rng, 0, innovation_models_[i]->sd());
    }
  }

  void SLLSM::simulate_initial_state(RNG &rng, VectorView eta) const {
    if (initial_state_mean_.size() != state_dimension()) {
      report_error("You need to set the mean and variance for "
                   "the initial state.");
    }
    eta = rmvn_mt(rng, initial_state_mean_, initial_state_variance_);
  }
  
  Ptr<SparseMatrixBlock> SLLSM::observation_coefficients(
      int t, const Selector &observed) const {
    if (!observation_coefficients_current_) {
      update_coefficients();
    }
    if (observed.nvars() == observed.nvars_possible()) {
      return observation_coefficients_;
    } else if (observed.nvars() == 0) {
      return empty_;
    } else {
      return new DenseMatrix(observed.select_rows(
          observation_coefficients_->dense()));
    }
  }

  void SLLSM::set_initial_state_mean(const Vector &mean) {
    if (mean.size() != state_dimension()) {
      report_error("Wrong size argument in set_initial_state_mean.");
    }
    initial_state_mean_ = mean;
  }

  void SLLSM::set_initial_state_variance(const SpdMatrix &variance) {
    if (variance.nrow() != state_dimension()) {
      report_error("Wrong size argument in set_initial_state_variance.");
    }
    initial_state_variance_ = variance;
    bool ok = true;
    initial_state_variance_cholesky_ = variance.chol(ok);
    if (!ok) {
      report_error("Variance is not positive definite in "
                   "set_initial_state_variance.");
    }
  }

  void SLLSM::update_complete_data_sufficient_statistics(
      int t, const ConstVectorView &state_error_mean,
      const ConstSubMatrix &state_error_variance) {
    report_error("update_complete_data_sufficient_statistics "
                 "is not implemented.");
  }

  void SLLSM::increment_expected_gradient(
      VectorView gradient, int t, const ConstVectorView &state_error_mean,
      const ConstSubMatrix &state_error_variance) {
    report_error("increment_expected_gradient is not implemented.");
  }

  void SLLSM::update_coefficients() const {
    if (!observation_coefficients_current_) {
      observation_coefficients_->set(coefficient_model_->Beta().transpose());
      observation_coefficients_current_ = true;
    }
  }

  void SLLSM::set_param_policy() {
    ParamPolicy::add_model(coefficient_model_);
    for (int i = 0; i < innovation_models_.size(); ++i) {
      ParamPolicy::add_model(innovation_models_[i]);
    }
  }

  void SLLSM::initialize_model_matrices() {
    // The multivariate regression model is organized as (xdim, ydim).  The 'X'
    // in our case is the state, where we want y = Z * state, so we need the
    // transpose of the coefficient matrix from the regression.
    observation_coefficients_.reset(new DenseMatrix(
        coefficient_model_->Beta().transpose()));

    if (!empty_) {
      empty_.reset(new EmptyMatrix);
    }
    state_transition_matrix_.reset(new IdentityMatrix(state_dimension()));

    state_variance_matrix_.reset(new DiagonalMatrixParamView);
    for (int i = 0; i < innovation_models_.size(); ++i) {
      state_variance_matrix_->add_variance(innovation_models_[i]->Sigsq_prm());
    }
    
  }

  // The logic here is :
  // Y = Z * alpha
  //   = Beta.tranpose() * alpha
  //   = (QR).transpose() * alpha
  //   = R.transpose() * Q.transpose * alpha
  // Thus, if we set Beta = R and pre_multiply alpha by Q.transpose then the
  // constraints will be satisfied.
  //
  // NOTE:  still need to scale by diag(R)
  void SLLSM::impose_identifiability_constraint() {
    Matrix Beta = coefficient_model_->Beta();
    QR BetaQr(Beta);
    Matrix R = BetaQr.getR();
    DiagonalMatrix Rdiag(R.diag());
    const Matrix &Q(BetaQr.getQ());
        
    coefficient_model_->set_Beta(BetaQr.getR());
    SubMatrix state = host_->mutable_full_state_subcomponent(index());
    Vector workspace(state.nrow());
    for (int i = 0; i < state.ncol(); ++i) {
      workspace = Q.Tmult(state.col(i));
      Rdiag.multiply_inplace(workspace);
      state.col(i) = workspace;
    }
    coefficient_model_->set_Beta(Rdiag.solve(R));
  }

  void SLLSM::set_observation_coefficients_observer() {
    std::function<void(void)> observer = [this]() {
      this->observation_coefficients_current_ = false;
    };
    coefficient_model_->Beta_prm()->add_observer(observer);
    observation_coefficients_current_ = false;
  }
  
}  // namespace BOOM
