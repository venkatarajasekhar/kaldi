/*
 * vts-est-noise.cc
 *
 *  Created on: Oct 30, 2012
 *      Author: Troy Lee (troy.lee2008@gmail.com)
 *
 *  One iteration of noise parameters estimation for first order VTS.
 *
 *  mode-in: clean model/canonical model
 *  alignments-rspecifier: alignment using the clean model and previous noise params
 *
 *  The difference between this program and the noise estimation in
 *  vts-model-decode is the reference transcription used in this one
 *  for noise estimation. However, for vts-model-decode, the hypotheses
 *  are generated using existing model.
 *
 */

#include "base/kaldi-common.h"
#include "util/common-utils.h"
#include "gmm/am-diag-gmm.h"
#include "decoder/decodable-am-diag-gmm.h"
#include "hmm/transition-model.h"
#include "fstext/fstext-lib.h"
#include "decoder/faster-decoder.h"
#include "util/timer.h"
#include "lat/kaldi-lattice.h" // for CompactLatticeArc
#include "gmm/diag-gmm-normal.h"

#include "vts/vts-first-order.h"

int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    typedef kaldi::int32 int32;

    const char *usage =
        "Estimate noise parameters for 1st-order VTS model compensation.\n"
        "Usage:  vts-est-noise [options] model-in features-rspecifier "
        "alignments-rspecifier noise-rspecifier noise-wspecifier\n"
        "Note: Features are MFCC_0_D_A, C0 is the last item.\n";
    ParseOptions po(usage);

    int32 num_cepstral = 13;
    int32 num_fbank = 26;
    BaseFloat ceplifter = 22;

    BaseFloat variance_lrate = 0.1;
    BaseFloat max_noise_mean_magnitude = 999999.0;

    po.Register("num-cepstral", &num_cepstral, "Number of Cepstral features");
    po.Register("num-fbank", &num_fbank,
                "Number of FBanks used to generate the Cepstral features");
    po.Register("ceplifter", &ceplifter,
                "CepLifter value used for feature extraction");

    po.Register("variance-lrate", &variance_lrate,
                "Learning rate for additive noise diagonal covariance");
    po.Register("max-noise-mean-magnitude",&max_noise_mean_magnitude,
                "Maximum magnitude value for the noise means (including"
                " both convolutional and additive noises)");

    po.Read(argc, argv);

    if (po.NumArgs() != 5) {
      po.PrintUsage();
      exit(1);
    }

    std::string model_rxfilename = po.GetArg(1),
        feature_rspecifier = po.GetArg(2),
        alignments_rspecifier = po.GetArg(3),
        noise_in_rspecifier = po.GetArg(4),
        noise_out_wspecifier = po.GetArg(5);

    TransitionModel trans_model;
    AmDiagGmm am_gmm;
    {
      bool binary;
      Input ki(model_rxfilename, &binary);
      trans_model.Read(ki.Stream(), binary);
      am_gmm.Read(ki.Stream(), binary);
    }

    SequentialBaseFloatMatrixReader feature_reader(feature_rspecifier);
    RandomAccessInt32VectorReader alignments_reader(alignments_rspecifier);
    RandomAccessDoubleVectorReader noiseparams_reader(noise_in_rspecifier);
    DoubleVectorWriter noiseparams_writer(noise_out_wspecifier);

    int num_success = 0, num_fail = 0;
    BaseFloat tot_update = 0.0, new_like = 0.0;

    Matrix<double> dct_mat, inv_dct_mat;
    GenerateDCTmatrix(num_cepstral, num_fbank, ceplifter, &dct_mat,
                      &inv_dct_mat);

    for (; !feature_reader.Done(); feature_reader.Next()) {
      std::string key = feature_reader.Key();
      Matrix<BaseFloat> features(feature_reader.Value());
      feature_reader.FreeCurrent();

      KALDI_VLOG(1) << "Current utterance: " << key;

      if (features.NumRows() == 0) {
        KALDI_WARN << "Zero-length utterance: " << key;
        num_fail++;
        continue;
      }

      int32 feat_dim = features.NumCols();
      if (feat_dim != 39) {
        KALDI_ERR << "Could not decode the features, only 39D MFCC_0_D_A is supported!";
      }

      /************************************************
       load alignment
       *************************************************/

      if (!alignments_reader.HasKey(key)) {
        KALDI_WARN << "No alignment could be found for "
            << key << ", utterance ignored.";
        ++num_fail;
        continue;
      }
      const std::vector<int32> &alignment = alignments_reader.Value(key);

      if (alignment.size() != features.NumRows()) {
        KALDI_WARN << "Alignments has wrong size " << (alignment.size())
            << " vs. " << (features.NumRows());
        ++num_fail;
        continue;
      }

      /************************************************
       load parameters for VTS compensation
       *************************************************/

      if (!noiseparams_reader.HasKey(key + "_mu_h")
          || !noiseparams_reader.HasKey(key + "_mu_z")
          || !noiseparams_reader.HasKey(key + "_var_z")) {
        KALDI_WARN
            << "Not all the noise parameters (mu_h, mu_z, var_z) are available!";
        ++num_fail;
        continue;
      }

      Vector<double> mu_h(noiseparams_reader.Value(key + "_mu_h"));
      Vector<double> mu_z(noiseparams_reader.Value(key + "_mu_z"));
      Vector<double> var_z(noiseparams_reader.Value(key + "_var_z"));

      // keep a copy of current estimate
      Vector<double> mu_h0(mu_h), mu_z0(mu_z), var_z0(var_z);

      // saved parameters for noise model computation
      std::vector<Matrix<double> > Jx(am_gmm.NumGauss()), Jz(am_gmm.NumGauss());
      Vector<double> gamma(am_gmm.NumGauss());  // sum( gamma_t_m )
      Matrix<double> gamma_p(am_gmm.NumGauss(), feat_dim);  // sum( gamma_t_m * y_t )
      Matrix<double> gamma_q(am_gmm.NumGauss(), feat_dim);  // sum( gamma_t_m * y_t * y_t )

      /************************************************
       Compensate the model
       *************************************************/

      // model after compensation
      AmDiagGmm noise_am_gmm;
      // Initialize with the clean speech model
      noise_am_gmm.CopyFromAmDiagGmm(am_gmm);

      CompensateModel(mu_h, mu_z, var_z, num_cepstral, num_fbank, dct_mat,
                      inv_dct_mat, noise_am_gmm, Jx, Jz);

      /*
       * Convert alignment to state posterior, which will be used for Gaussian posterior
       * computation.
       */

      BaseFloat tot_like_this_file = AccumulatePosteriorStatistics(noise_am_gmm,
                                                                   trans_model,
                                                                   alignment,
                                                                   features,
                                                                   gamma,
                                                                   gamma_p,
                                                                   gamma_q);

      bool new_mean_estimate = false;
      bool new_var_estimate = false;

      /*
       * Estimate noise means, mu_h, mu_z (only have static coefficients)
       */
      SubVector<double> mu_h_s(mu_h, 0, num_cepstral), mu_z_s(mu_z, 0,
                                                              num_cepstral);
      EstimateStaticNoiseMean(noise_am_gmm, gamma, gamma_p, gamma_q, Jx, Jz,
                              num_cepstral, max_noise_mean_magnitude, mu_h_s,
                              mu_z_s);

      if (g_kaldi_verbose_level >= 1) {
        KALDI_LOG << "New Additive Noise Mean: " << mu_z;
        KALDI_LOG << "New Convoluational Noise Mean: " << mu_h;
      }

      new_mean_estimate = BackOff(am_gmm, trans_model, alignment, features,
                                  num_cepstral, num_fbank, dct_mat, inv_dct_mat,
                                  mu_h0, mu_z0, var_z0, mu_h, true, mu_z, true,
                                  var_z, false, noise_am_gmm, Jx, Jz);

      if (fabs(variance_lrate) > 1e-6) {
        /*
         * Estimate noise variance, have static, delta and accelerate parameters.
         * if variance_lrate == 0, then no variance estimation.
         */
        EstimateAdditiveNoiseVariance(noise_am_gmm, gamma, gamma_p, gamma_q, Jz,
                                      num_cepstral, feat_dim, variance_lrate,
                                      var_z);

        new_var_estimate = BackOff(am_gmm, trans_model, alignment, features,
                                   num_cepstral, num_fbank, dct_mat,
                                   inv_dct_mat, mu_h0, mu_z0, var_z0, mu_h,
                                   false, mu_z, false, var_z, true,
                                   noise_am_gmm, Jx, Jz);
      }

      if (!new_mean_estimate && !new_var_estimate){
        KALDI_WARN << "No updates for " << key;
      } else {
        new_like = ComputeLogLikelihood(noise_am_gmm,
                                       trans_model,
                                       alignment,
                                       features);
        tot_update += (new_like - tot_like_this_file);

      }

      if (g_kaldi_verbose_level >= 1) {
        KALDI_LOG << "Final Additive Noise Mean: " << mu_z;
        KALDI_LOG << "Final Additive Noise Covariance: " << var_z;
        KALDI_LOG << "Final Convoluational Noise Mean: " << mu_h;
      }

      // Writting the noise parameters out
      noiseparams_writer.Write(key + "_mu_h", mu_h);
      noiseparams_writer.Write(key + "_mu_z", mu_z);
      noiseparams_writer.Write(key + "_var_z", var_z);

      ++num_success;

      if(num_success % 100 == 0){
        KALDI_LOG << "Done " << num_success << " utterances. Log-likelihood increase for " << key
            << " is " << (new_like - tot_like_this_file) << " over " << features.NumRows() << " frames.";
      }

    }

    KALDI_LOG << "Done " << num_success << " utterances, failed for "
        << num_fail;
    KALDI_LOG << "Overall log-likelihood increase per file is "
        << (tot_update / num_success) << " over " << num_success << " files.";

    return (num_success != 0 ? 0 : 1);
  } catch (const std::exception &e) {
    std::cerr << e.what();
    return -1;
  }
}

