import BayesBoom as boom
import numpy as np
import pandas as pd
import patsy
import spikeslab
import R
import scipy.sparse
from abc import ABC, abstractmethod
from .state_models import StateModel


class Bsts:
    """
    Bayesian structural time series models.  Any other "BS" is the fault of the
    analyst!!

    Bsts supports models for scalar time series, where the outcome variable is
    either conditionally Gaussian, student T, binomial (logit linke), or
    Poisson (log link).  A key feature of bsts models is optional support for
    contemporaneous covariates through a Bayesian spike-and-slab prior that
    handles model selection, averaging, and uncertainty.

    This class is a port of the bsts R package.  The underlying C++ code for
    the two packages is the same, but the interfaces are slightly different to
    reflect seemingly "natural" approaches in the two languages.

    Expected usage:
    data = get_time_series_from_somewhere()  # A pd.Series
    model = Bsts()
    model.add_state(LocalLinearTrend(data))
    model.add_state(SeasonalStateModel(data))
    model.train(niter=1000)

    model.plot()
    model.plot("coef")
    model.plot("coefficients")
    model.plot("comp")

    pred = model.predict(12)
    pred.plot()
    pred.posterior_mean()
    """

    def __init__(self, family: str = "gaussian", seed: int = None):
        """
        Create an "empty" bsts model.

        Args:

          family: The type of error distribution for the outcome variable.
            This will eventually support gaussian, poisson, student, and
            binomial errors.  For now only "gaussian" is supported, but the
            others are on the roadmap.

          seed:
            An integer (or None) containingt the random seed for the C++ random
            number generator.
        """
        # The model family.
        supported_families = ["gaussian", "student", "binomial", "poisson"]
        self._family = R.unique_match(family.lower(), supported_families)

        if seed is not None:
            boom.GlobalRng.rng.seed(seed)

        # self._model is the BayesBoom handle to the BOOM C++ state space model
        # object.

        self._model = None
        # The 'observation model' lives inside self._model.  The job of the
        # observation model manager is to handle specific cases of observation
        # models in terms of housekeeping like recording MCMC draws.
        self._observation_model_manager = None

        # self._state_models is a list of StateModel objects that reflect the
        # state models in self._model.  Their job is similar to that of the
        # observation_model_manager.
        self._state_models = []

        # The dimension of the latent state.
        self._state_dimension = 0

    @property
    def time_dimension(self):
        return self._model.time_dimension

    def add_state(self, state_model: StateModel):
        """
        Add a component of state to the model.

        Args:
          state_model: A StateModel object describing the state to be added.
            Examples of class StateModel include LocalLevel and
            LocalLinearTrend models for trend, Seasonal or Trig state models
            for modeling seaonality, and various holiday state models for
            describing holiday effects.

        Effects:
          The state model is appended to self._state_models, and the state model
          is informed about the position of the state it manages in the global
          state vector.
        """
        self._state_models.append(state_model)
        state_model.set_state_index(self._state_dimension)
        self._state_dimension += state_model.state_dimension

    def train(self, data, niter: int, formula=None, prior=None, ping: int = None):
        """
        Train a bsts model by running a specified number of MCMC iterations.

        Args:
          formula: Either numeric time series (array-like), or a string giving
            a formula that can be interpreted by the 'patsy' package (python's
            version of R's model syntax).
          data: If a formula is given, data is a DataFrame containing the
            variables from the formula.  If 'formula' is a numeric then 'data'
            need not be specified.
          prior: The prior distribution for the observation model.  If a
            regression component is included then this is a
            spikeslab.RegressionSpikeSlabPrior describing the regression
            coefficients and the residual standard deviation.  Otherwise it is
            a boom.SdPrior on the residual standard deviation.  If None then a
            default prior will be chosen.
          niter:  The desired number of MCMC iterations.
          ping: The frequency with which to print status updates in the MCMC
            algorithm.  The default is niter/10.  If ping <= 0 then no status
            updates are printed.

        Effects:
          The model is populated with MCMC draws.  The regression parameters,
          if any, are stored in the model object itself.  Parameters for any
          state models are stored in the state model objects.  The
          contributions of each state model are stored in the state model
          objects.
        """
        self._create_model(formula, data, prior)
        self._allocate_space(niter)

        for i in range(niter):
            self._model.sample_posterior()
            self._record_draws(i)

    def plot(self, what=None, **kwargs):
        plot_types = ["state", "components", "coefficients", "inclusion",
                      "residuals", "prediction_errors",
                      "forecast_distribution", "predictors",
                      "size", "dynamic", "seasonal", "monthly",
                      "help"]
        if what is None:
            what = "state"
        what = R.unique_match(what.lower(), plot_types)
        if what == "state":
            self.plot_state(**kwargs)
        elif what == "components":
            self.plot_state_components(**kwargs)
        elif what == "coefficients":
            self.plot_coefficients(**kwargs)
        elif what == "inclusion":
            self.plot_inclusion_probs(**kwargs)
        elif what == "residuals":
            self.plot_residuals(**kwargs)
        elif what == "prediction_errors":
            self.plot_prediction_errors(**kwargs)
        elif what == "forecast_distribution":
            self.plot_forecast_distribution(**kwargs)
        elif what == "predictors":
            self.plot_predictors(**kwargs)
        elif what == "size":
            self.plot_model_size(**kwargs)
        elif what == "dynamic":
            self.plot_dynamic_regression(**kwargs)
        elif what == "seasonal":
            self.plot_seasonal(**kwargs)
        elif what == "monthly":
            self.plot_monthly(**kwargs)
        elif what == "help":
            pass
        else:
            raise Exception(f"Don't know how to plot {what}.")

    def predict(self, horizon, newdata):
        """
        Returns:
            A BstsPrediction object containing the predictions.
        """
    def _record_draws(self, iteration: int):
        """
        Record the parameters and state from each state model.

        Args:
          iteration: The iteration (MCMC draw) number being recorded.
        """
        state_matrix = self._model.state.to_numpy()
        for m in self._state_models:
            m.record_state(iteration, state_matrix)

        self._observation_model_manager.record_draw(
            iteration, self._model)

    def _allocate_space(self, niter: int):
        """
        Allocate space in the model for 'niter' MCMC draws.
        """
        self._observation_model_manager.allocate_space(niter)
        for state_model in self._state_models:
            state_model.allocate_space(niter, self.time_dimension)

    def _create_model(self, formula, data, prior):
        """Create the boom model object.

        Args:
          formula: Either numeric time series (array-like), or a string giving
            a formula that can be interpreted by the 'patsy' package (python's
            version of R's model syntax).
          data: If a formula is given, data is a DataFrame containing the
            variables from the formula.  If 'formula' is a numeric then 'data'
            need not be specified.
          prior: The prior distribution for the observation model.  If a
            regression component is included then this is a
            spikeslab.RegressionSpikeSlabPrior describing the regression
            coefficients and the residual standard deviation.  Otherwise it is
            a boom.SdPrior on the residual standard deviation.  If None then a
            default prior will be chosen.

        Effects:
          self._model is created, populated with data and assigned a posterior
            sampler.
        """
        factory = StateSpaceModelFactory.create(self._family, formula)
        self._model = factory.create_model(prior, data)
        self._observation_model_manager = (
            factory.create_observation_model_manager()
        )
        for state_model in self._state_models:
            self._model.add_state(state_model._state_model)



class BstsPrediction:
    """
    Posterior predictive distribution produced by the call to Bsts.pred.
    """


class ObservationModelManager(ABC):
    """
    Manages making space for and recording MCMC draws for specific families of
    observation models.
    """

    @abstractmethod
    def allocate_space(self, niter: int, xdim: int):
        """
        Create space for 'niter' MCMC draws of the observation model parameters.
        This will include regression coefficients if the observation model has
        been assigned a formula.
        """

    @abstractmethod
    def record_draw(self, iteration, model):
        """
        Record the model parameters at the given iteration.
        """


class GaussianStateSpaceModelFactory:

    def __init__(self):
        self._model = None

    def create_model(self, prior: R.SdPrior, data: pd.Series):
        """
        Args:
          prior: an R.SdPrior object describing the prior distribution on the
            residual variance paramter.
          data:  The time series of observations.
        """
        boom_data = boom.Vector(data.values)
        is_observed = ~data.isna()
        self._model = boom.StateSpaceModel(boom_data, is_observed)

        if prior is None:
            sdy = np.std(data)
            prior = R.SdPrior(sigma_guess=sdy, upper_limit=sdy * 1.2)

        boom_prior = boom.ChisqModel(prior.sample_size, prior.sigma_guess)
        observation_model_sampler = boom.ZeroMeanGaussianConjSampler(
            self._model.observation_model,
            boom_prior)
        observation_model_sampler.set_sigma_upper_limit(
            prior.upper_limit)
        self._model.observation_model.set_method(observation_model_sampler)

        sampler = boom.StateSpacePosteriorSampler(
            self._model, boom.GlobalRng.rng)
        self._model.set_method(sampler)

        return self._model

    def create_observation_model_manager(self):
        return GaussianObservationModelManager(xdim=0)


class StateSpaceModelFactory(ABC):
    @staticmethod
    def create(family, formula):
        family = R.unique_match(
            family.lower(),
            ["gaussian", "student", "binomial", "poisson"])

        if family == "gaussian" and formula is None:
            return GaussianStateSpaceModelFactory()
        elif family == "gaussian":
            return StateSpaceRegressionModelFactory(formula)
        elif family == "student":
            return StateSpaceStudentModelFactory(formula)
        elif family == "poisson":
            return StateSpacePoissonModelFactory(formula)
        elif family == "binomial":
            return StateSpaceLogitModelFactory(formula)
        else:
            raise Exception(f"Unrecognized family {family}.")

    @abstractmethod
    def create_model(self, prior, data):
        """
        Create the BOOM model object.  The prior is assigned to the observation
        model, and the data is assigned to the model.  State will be assigned
        later.

        Args:
          prior: A prior distribution appropriate to the type of observation
            model.  Child classes will make and document specific assumptions
            about which priors are appropriate.
          data: The training data for the model.  In most cases this will be a
            pd.DataFrame (if the model contains a regression component) or a
            pd.Series otherwise.  The index of the data may contain timestamps.
        """

    @abstractmethod
    def create_observation_model_manager(self):
        """
        Return an ObservationModelManager object appropriate to the concrete
        observation model.
        """


class GaussianObservationModelManager:
    def __init__(self, xdim):
        self._xdim = xdim

    def allocate_space(self, niter: int):
        self._residual_sd = np.empty(niter)

        if self._xdim > 0:
            self._coefficients = scipy.sparse.lil_matrix(
                (niter, self._xdim))

    def record_draw(self, iteration: int, model):
        self._residual_sd[iteration] = model.residual_sd
        if self._xdim > 0:
            self._coefficients[iteration, :] = spikeslab.lm_spike.sparsify(
                model.coef)


class StateSpaceRegressionModelFactory:

    def __init__(self, formula):
        assert isinstance(formula, str)
        self._formula = formula

    def create_model(self, prior, data):
        """Create the boom model object, and store related model artifacts.

        Args:
          formula:  A model formula describing the regression component.
          data: A pandas DataFrame containing the variables appearing
            'formula'.
          prior: A spikeslab.RegressionSpikeSlabPrior describing the prior
            distribution on the regression coefficients and the residual
            standard deviation.

        Effects: self._model is created, and model formula artifacts are stored
          so they will be available for future predictions.

        """
        assert isinstance(prior, spikeslab.RegressionSpikeSlabPrior)
        response, predictors = patsy.dmatrices(self._formula, data)
        is_observed = ~np.isnan(response)

        self._model = boom.StateSpaceRegressionModel(
            boom.Vector(response),
            boom.Matrix(predictors),
            is_observed)

        spikeslab.set_posterior_sampler(self._model.observation_model, prior)

    def create_observation_model_manager(self):
        return GaussianObservationModelManager(self._model.xdim)
