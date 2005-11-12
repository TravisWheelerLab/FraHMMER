/* modelconfig.c
 * Model configuration: the process of converting a core model to a
 * fully configured Plan7 HMM with an "algorithm mode" built into it.
 * 
 * Revised, refactored May 2005: xref STL9/77-81.  (Uniform fragment length distribution)
 * And then again, Sept 2005:    xref STL10/24-26. (Inherent target length dependency)
 * 
 * SRE, Mon May  2 10:55:16 2005 [St. Louis]
 * SVN $Id$
 */

#include "config.h"		/* must be included first */
#include "squidconf.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include "squid.h"

#include "plan7.h"		/* the model structure */
#include "funcs.h"
#include "structs.h"

static void config_g (struct plan7_s *hmm);
static void config_s (struct plan7_s *hmm);
static void config_ls(struct plan7_s *hmm);
static void config_sw(struct plan7_s *hmm);
static void config_fs(struct plan7_s *hmm);

static void left_wing_retraction_imposed(struct plan7_s *hmm);
static void left_wing_retraction_added(struct plan7_s *hmm);
static void right_wing_retraction_imposed(struct plan7_s *hmm);
static void right_wing_retraction_added(struct plan7_s *hmm);

static void local_dpath_accounting(struct plan7_s *hmm);
static void normal_delete_scores(struct plan7_s *hmm);

static void target_ldependence(struct plan7_s *hmm, int L);

static void logoddsify_the_rest(struct plan7_s *hmm);


/* Function:  P7Config()
 * Incept:    SRE, Sun Sep 25 12:21:25 2005 [St. Louis]
 *
 * Purpose:   Given a model <hmm>, with core probabilities set and null
 *            model probabilities set, and a mode <mode> (one of
 *            <P7_LS_MODE>, <P7_FS_MODE>, <P7_SW_MODE>, <P7_S_MODE>,
 *            <P7_G_MODE>); configure the model into the appropriate
 *            scoring form for that algorithm mode.
 *            
 *            Specifically, all scores <tsc>,<msc>,<xsc>,<bsc>,<esc>
 *            are set by this call, and the <PLAN7_HASBITS> flag is
 *            raised on the model.
 *            
 *            The model is not configured yet for a specific target
 *            sequence length.  (By default, it is set to whatever the
 *            expectation of the null model was.)  To reconfigure both
 *            the HMM and the null model for a specific target length,
 *            call <P7ReconfigLength()>.
 *            
 *            The configuration-dependent model probability params are
 *            also set (N,E,C,J,B states), and the <M->E> end
 *            probabilities are set, for reference only. (Because the
 *            main model transition probabilities may be implicitly
 *            renormalized as scores are calculated, because of added
 *            <M->E> transitions, wing retraction, and d-path
 *            leveling, the HMM structure does not store an explicit
 *            full probabilistic model of the search-configured HMM;
 *            the probabilistic model is implicit in the log-odds
 *            scores.) The caller only needs to be aware of this if it
 *            intends to do something with the search-configured
 *            probabilistic model.
 *            
 *            If necessary (for numerical reasons), the <PLAN7_LCORRECT>
 *            flag will be raised on the model. This indicates that we
 *            lack sufficient numeric precision to represent transition scores
 *            for the unaligned residues, so their contribution (a total
 *            of ~1 bit for single-hit mode, ~2 bits for default multihit 
 *            mode) must instead be added post hoc to a sequence score.
 *            [SRE_FIXME: xref to the approp function here]
 */
void
P7Config(struct plan7_s *hmm, enum p7_algmode mode)
{
  switch (mode) {
  case P7_NO_MODE: Die("No mode selected."); break;
  case P7_LS_MODE: config_ls(hmm);           break;
  case P7_FS_MODE: config_fs(hmm);           break;
  case P7_SW_MODE: config_sw(hmm);           break;
  case P7_S_MODE:  config_s(hmm);            break;
  case P7_G_MODE:  config_g(hmm);            break; /* "naked", pure global alignment mode */
  default:         Die("Bad mode.");
  }
}


/* Function:  P7ReconfigLength()
 * Incept:    SRE, Sun Sep 25 12:38:55 2005 [St. Louis]
 *
 * Purpose:   Given a model already configured for scoring, in some  
 *            particular algorithm mode; 
 *            reset the expected length distribution of the HMM
 *            and the null model to a mean of <L>.
 *            
 *            Do this as quickly as possible, so that the caller
 *            can dynamically reconfigure the model for the length
 *            of each target sequence in a database search.
 */
void
P7ReconfigLength(struct plan7_s *hmm, int L)
{
  target_ldependence(hmm, L);
}


/* Function:  P7FinalBitscore()
 * Incept:    SRE, Sun Sep 25 14:10:31 2005 [St. Louis]
 *
 * Purpose:   Given an integer lod score <sc> from an alignment
 *            algorithm, apply any necessary posthoc correction to it and
 *            return the score in bits (as a float). All alignment
 *            algorithms that return a bit score should translate
 *            the integer lod score to bits using this function.
 *            
 *            Currently the only correction is a numerical tweak
 *            to the length correction, if the <PLAN7_LCORRECT> 
 *            flag is up. 
 *            
 *            This is distinct from other corrections, like the
 *            TraceScoreCorrection(). 
 */
float
P7FinalBitscore(struct plan7_s *hmm, float sc, int L)
{
  float Lp;

  if (hmm->flags & PLAN7_LCORRECT)
    {
      Lp = (float) L - hmm->kappa;
      if (Lp < 1.) Lp = 1.;
      sc += hmm->lscore * Lp;
    }
  return sc;
}

/******************************************************************
 * General notes on model configuration.
 * 
 * When you enter this module, you've got an HMM in "core" probability
 * form: t[], mat[], ins[], and tbd1 are all valid, normalized
 * probabilities. The routines here are used to create the
 * "configured" score form of the model: tsc[], msc[], isc[], bsc[],
 * esc[], and xsc[] fields become valid integer log-odds scores. 
 * 
 * Also in the process, xt[], begin[], and end[] are set to their
 * algorithm-dependent probabilities, though these probabilities are
 * only for reference.  Because of the way the main model's
 * probabilities must be modified to deal with entry/exit
 * probabilities, and because we don't want to modify the core
 * probabilities, we don't ever quite store the full probabilistic
 * model of the configured model, only its log odds scores.
 * 
 * The configuration process breaks down into distinct conceptual steps:
 * 
 * 1. Algorithm configuration.
 *    An "algorithm mode" is chosen. This sets the probabilities in
 *    xt[XTE], begin[], and end[], which determine local/global and
 *    multihit/single hit behavior in an HMM alignment to a target
 *    sequence. The "nj" value of the HMM is also set here (the expected
 *    # of times the J state will be used; 0 for single-hit mode and
 *    1 for the default parameterization of multihit modes).
 *    
 * 2. Wing retraction.
 *    In a configured model, the D_1 and D_M states are removed.
 *    The probability of the paths B->D1...->Mk ("BMk") that enter
 *    D1 and use all D's before reaching M_k is treated instead
 *    as an additional dollop of B->Mk entry probability, and the
 *    probability of paths Mk->Dk+1...D_M->E ("MkE") is treated
 *    instead as an additional dollop of Mk->E exit probability.
 *    The MkE path probability is subtracted from the Mk->Dk+1
 *    transition.
 *    
 *    If the algorithm mode is imposing its own local entry/exit
 *    probabilities, then these extra dollops are ignored, and the
 *    model is renormalized appropriately. That is, the algorithm
 *    overrides all B->DDDD->M and/or M->DDDD->E path probabilities
 *    with its own internal entry/exit probabilities.
 *    
 *    If the algorithm mode is "global" at either entry or exit, then
 *    the internal entries are set to BMk and internal exits are set
 *    to MkE, and the model is renormalized appropriately.  That is,
 *    the algorithm treats B->DDDD->M and/or M->DDDD->E path
 *    probabilities as internal entries/exits, instead of allowing
 *    dynamic programming algorithms to use the D_1 or D_M states.
 *    
 *    DP traceback algorithms check whether the model is imposing
 *    internal entry/exit, and construct tracebacks accordingly;
 *    either leaving B->Mk/Mk->E transitions as is, or interpreting
 *    them as B->DDDD->Mk/Mk->DDDD->E paths.
 *    
 *    Wing retraction has two purposes. First, it removes a mute cycle
 *    from the model, B->D1 ...D_M->E, which cannot be correctly and
 *    efficiently dealt with by DP recursions. (A DP algorithm could
 *    simply *ignore* that path though, and throw away the negligible
 *    amount of probability in it. Wing retraction is an elaborate,
 *    overly stylistic alternative that keeps everything fully
 *    probabilistic. Second, it reconciles the algorithm-dependent
 *    entry/exit probabilities with the core model. For algorithms
 *    that impose particular internal entry/exit, you don't want there
 *    to be any additional probability coming from "internal"
 *    B->DDD->M and M->DDD->E paths, so wing retraction takes it away.
 *    
 * 3. Local alignment D-path leveling.
 *    For fully local alignments, we want every fragment ij (starting
 *    at match i, ending from match j) to be equiprobable. There are
 *    M(M+1)/2 possible such fragments, so the probability of each
 *    one is 2/M(M+1). 
 *    
 *    Notionally, we imagine a "model" consisting of the M(M+1)/2
 *    possible fragments, with entry probability of 2/M(M+1) for each.
 *    
 *    Operationally, we achieve this by a trick inspired by a
 *    suggestion from Bill Bruno. Bill suggested that for a model with
 *    no delete states, if we set begin[k] = 1/(M-k+1) and end[k] =
 *    (M-k+1) / [M(M+1)/2], all fragments are equiprobable: the prob
 *    of any given fragment is
 *         b_i * e_j * \prod_{k=i}^{j-1} (1-e_k);
 *    that is, the fragment also includes (j-i) penalizing terms for
 *    *not* ending at i..j-1. Remarkably, this gives the result we
 *    want: this product is always 2/M(M+1), for any ij.
 *    
 *    D->D transitions throw a wrench into this trick, though. A local
 *    alignment that goes M_i->D...D->M_j, for example, only gets
 *    hit with one not-end penalty (for the M_i->D). This means that
 *    paths including deletions will be artifactually favored. 
 *    
 *    The solution is to subtract log(1-e_k) from the deletion
 *    transition scores as well as the match transition scores.  Thus
 *    one log(1-e_k) penalty is always exacted upon transitioning from
 *    any node k->k+1. This is *not* part of the probabilistic model:
 *    it is a score accounting trick that forces the DP algorithms to
 *    associate a log(1-e_k) penalty for each node k->k+1 transition,
 *    which makes the DP calculations give the result desired for our
 *    *notional* probabilistic model with a single 2/M(M+1) transition
 *    for each possible fragment. (A similar accounting trick is the
 *    use of log-odds scoring, where we associate null model
 *    transitions and emissions with appropriate terms in the HMM, to
 *    assure that the final score of any path accounts for all the
 *    desired probability terms in an overall log-odds score). The
 *    overall score of any fragment can be rearranged such that there
 *    is one term consisting of a product of all these penalties * b_i
 *    * e_j = 2/M(M+1), and another term consisting of the actual
 *    model transition path score between i,j.
 *    
 * 4. Target length dependence. 
 *    Given a particular target sequence of length L, we want our HMM score
 *    to be as independent as possible of L. Otherwise, long sequences will
 *    give higher scores, even if they are nonhomologous. 
 *    
 *    The traditional solution to this is Karlin/Altschul statistics,
 *    which tells us that E(s=x) = KMNe^-{\lambda x}, so we expect to
 *    have to make a -1 bit score correction for every 2x increase in
 *    target sequence length (ignoring edge correction effects). K/A
 *    statistics have only been proven for local Viterbi single-hit
 *    ungapped alignments. They have been shown to hold empirically
 *    for local Viterbi single-hit gapped alignments. In my hands the. In my hands 
 *    they also hold for any single-hit alignment (local or glocal, Viterbi
 *    or forward) but they do not hold for multihit alignment modes.
 *    
 *    An alternative solution is to build the length dependence right
 *    into the probabilistic model, since we have a full probabilistic
 *    model of the target sequence. We match the expected lengths of
 *    the model M and the null model R by setting the p1, N, C, and J
 *    transitions appropriately. R has to emit the whole sequence, so
 *    it has a self-transition of L/(L+1). N, C, and J have to emit
 *    (L-(k+1)x) residues of the sequence, where x is the expected length
 *    of an alignment to the core model, and k is the expected number
 *    of times that we cycle through the J state. k=0 in sw mode, and k=1
 *    in fs/ls mode w/ the standard [XTE][LOOP] probability of 0.5. 
 *    
 * 5. Conversion of probabilities to integer log-odds scores.
 *    This step incorporates the contribution of the null model,
 *    and converts floating-point probs to the scaled integer log-odds
 *    score values that are used by the DP alignment routines. 
 *    
 * Step 1 is done by the main Plan7*Config() functions, which come in sw,
 * ls, fs, s, and "naked" flavors.
 *
 * Step 2 is done by the *wing_retraction*() functions, which also
 *  go ahead and convert the affected transitions to log-odds scores;
 *  left wing retraction sets bsc[], right wing retraction sets
 *  esc[] and tsc[TM*].
 *  
 * Step 3 is carried out by one of two delete path accounting routines,
 *  which go ahead and set tsc[TD*].
 *  
 * Step 4 is carried out by the target_ldependence() routine.
 * 
 * Step 5 is carried out for all remaining scores by logoddsify_the_rest().   
 * 
 * The model never exists in a configured probability form. The core
 * model is in probabilities; the configured model is in scores. In
 * fact, because a fully local model uses delete path score
 * accounting, you shouldn't even try to backcalculate probabilities
 * from these scores, at least for local models -- the probabilistic model
 * underlying fully local alignment is an implicit one that cannot
 * be represented in the profile HMM structure.
 *
 * So, overall, to find where the various scores and probs are set:
 *   bsc      :  wing retraction          (section 2)
 *   esc      :  wing retraction          (section 2)
 *   tsc[TM*] :  wing retraction          (section 2)
 *   tsc[TI*] :  logoddsify_the_rest()    (section 4)
 *   tsc[TD*] :  dpath leveling           (section 3)
 *   p1       :  target_ldependence()     (section 4)  
 *   xt[NCJ]  :  target_ldependence()     (section 4)  
 *   xsc (all):  logoddsify_the_rest()    (section 4)
 *   msc      :  logoddsify_the_rest()    (section 5)
 *   isc      :  logoddsify_the_rest()    (section 5)
 *****************************************************************
 */



/*****************************************************************
 * Section 1. The API: plan7's algorithm mode configuration functions.
 *
 * The following few functions are the Plan7 equivalent of choosing
 * different alignment styles (fully local, fully global,
 * global/local, multihit, etc.)
 * 
 * When you come into a configuration routine, the following
 * probabilities are valid in the model:
 *    1. t[1..M-1][0..6]: all the state transitions.
 *       (Node M is special: it has only a match and a delete state,
 *       no insert state, and M_M->E = 1.0 and D_M->E = 1.0 by def'n.)
 *    2. mat[1..M][]:  all the match emissions.
 *    3. ins[1..M-1][]: all the insert emissions. Note that there is
 *       no insert state in node M.
 *    4. tbd1: the B->D1 probability. The B->M1 probability is 1-tbd1.
 * These are the "data-dependent" probabilities in the model.
 * 
 * The configuration routine gets to set the "algorithm-dependent"
 * probabilities:
 *    1. xt[XTN][MOVE,LOOP] dist controls unaligned N-terminal seq.
 *       The higher xt[XTN][LOOP] is, the more unaligned seq we allow.
 *       Similarly, xt[XTC][MOVE,LOOP] dist controls unaligned C-terminal 
 *       seq, and xt[XTJ][MOVE,LOOP] dist controls length of unaligned sequence
 *       between multiple copies of a domain. Normally, if these are nonzero,
 *       they are all set to be equal to hmm->p1, the loop probability
 *       for the null hypothesis (see below).
 *    2. xt[XTE][MOVE,LOOP] dist controls multihits. 
 *       Setting xt[XTE][LOOP] to 0.0 forces one hit per model.
 *    3. begin[1..M] controls entry probabilities. An algorithm 
 *       mode either imposes internal begin probabilities, or leaves begin[1] as
 *       1.0 and begin[k] = 0.0 for k>1.
 *    4. end[1..M] controls exit probabilities. An algorithm mode either
 *       imposes internal exit probabilities, or leaves end[M] = 1.0
 *       and end[k] = 0.0 for k<M.
 *    
 * The configuration routine then calls routines as appropriate to set
 * up all the model's scores, given these configured probabilities. When
 * the config routine returns, all scores are ready for alignment:
 * bsc, esc, tsc, msc, isc, and xsc.
 * 
 *****************************************************************
 *
 * There is (at least) one more issue worth noting.
 * If you want per-domain scores to sum up to per-sequence scores, which is
 * generally desirable if you don't want "bug" reports from vigilant users,
 * then one of the following two sets of conditions must be met:
 *   
 *   1) t(E->J) = 0    
 *      e.g. no multidomain hits
 *      
 *   2) t(N->N) = t(C->C) = t(J->J) = hmm->p1 
 *      e.g. unmatching sequence scores zero, and 
 *      N->B first-model score is equal to J->B another-model score.
 *      
 * These constraints are obeyed in the default Config() functions below,
 * but in the future (say, when HMM editing may be allowed) we'll have
 * to remember this. Non-equality of the summed domain scores and
 * the total sequence score is a really easy "red flag" for people to
 * notice and report as a bug, even if it may make probabilistic
 * sense not to meet either constraint for certain modeling problems.
 *****************************************************************
 */



/* config_g()
 * 
 * Purpose:  Set the alignment-independent, algorithm-dependent parameters
 *           of a Plan7 model so that no special states (N,C,J) emit anything:
 *           one simple, full global pass through the model.
 * 
 * Args:     hmm - the plan7 model
 *                 
 * Return:   (void)
 *           The HMM is modified; algorithm dependent parameters are set.
 *           Previous scores are invalidated if they existed.
 */
static void
config_g(struct plan7_s *hmm)
{
  hmm->xt[XTE][MOVE] = 1.;	      /* only 1 domain/sequence ("global" alignment) */
  hmm->xt[XTE][LOOP] = 0.;
  hmm->xt[XTN][MOVE] = 1.;	      /* disallow N-terminal tail */
  hmm->xt[XTN][LOOP] = 0.;
  hmm->xt[XTC][MOVE] = 1.;	      /* disallow C-terminal tail */
  hmm->xt[XTC][LOOP] = 0.;
  hmm->xt[XTJ][MOVE] = 0.;	      /* J state unused */
  hmm->xt[XTJ][LOOP] = 1.;

  hmm->nj    = 0.;		      /* avg. usage of J state per path       */

  FSet(hmm->begin+2, hmm->M-1, 0.);   /* disallow internal entries. */
  hmm->begin[1]    = 1.0;
  FSet(hmm->end+1,   hmm->M-1, 0.);   /* disallow internal exits. */
  hmm->end[hmm->M] = 1.0;

  left_wing_retraction_added(hmm);
  right_wing_retraction_added(hmm);
  normal_delete_scores(hmm);
  target_ldependence(hmm, 400);
  logoddsify_the_rest(hmm);

  hmm->mode   = P7_G_MODE; 	 /* true global mode */
  hmm->flags &= ~PLAN7_BIMPOSED; /* no internal entry -> entry comes from wing retraction  */
  hmm->flags &= ~PLAN7_EIMPOSED; /* no internal exit  -> exit comes from wing retraction */
  hmm->flags |= PLAN7_HASBITS;   /* we're configured. */
}
   
/* config_s()
 * 
 * Purpose:  Set the alignment-independent, algorithm-dependent parameters
 *           of a Plan7 model to global (Needleman/Wunsch) configuration:
 *           "s" mode (hmms).
 * 
 *           Like a non-looping hmmls, since we actually allow flanking
 *           N and C terminal sequence. 
 *           
 * Args:     hmm - the plan7 model
 *                 
 * Return:   (void)
 *           The HMM is modified; algorithm dependent parameters are set.
 *           Previous scores are invalidated if they existed.
 */
static void
config_s(struct plan7_s *hmm)
{
  hmm->xt[XTN][MOVE] = 1. - hmm->p1;  /* allow N-terminal tail */
  hmm->xt[XTN][LOOP] = hmm->p1;
  hmm->xt[XTE][MOVE] = 1.;	      /* only 1 domain/sequence ("global" alignment) */
  hmm->xt[XTE][LOOP] = 0.;
  hmm->xt[XTC][MOVE] = 1. - hmm->p1;  /* allow C-terminal tail */
  hmm->xt[XTC][LOOP] = hmm->p1;
  hmm->xt[XTJ][MOVE] = 0.;	      /* J state unused */
  hmm->xt[XTJ][LOOP] = 1.;

  hmm->nj    = 0.;

  FSet(hmm->begin+2, hmm->M-1, 0.);   /* disallow internal entries. */
  hmm->begin[1]    = 1.0;
  FSet(hmm->end+1,   hmm->M-1, 0.);   /* disallow internal exits. */
  hmm->end[hmm->M] = 1.0;

  left_wing_retraction_added(hmm);
  right_wing_retraction_added(hmm);
  normal_delete_scores(hmm);
  target_ldependence(hmm, 400);
  logoddsify_the_rest(hmm);

  hmm->mode   = P7_S_MODE; 	 /* hmms mode */
  hmm->flags &= ~PLAN7_BIMPOSED; /* no internal entry -> entry comes from wing retraction  */
  hmm->flags &= ~PLAN7_EIMPOSED; /* no internal exit  -> exit comes from wing retraction */
  hmm->flags |= PLAN7_HASBITS;   /* we're configured */
}
   
/* config_ls()
 * 
 * Purpose:  Set the alignment independent parameters of a Plan7 model
 *           to hmmls (global in HMM, local in sequence) configuration.
 *           
 * Args:     hmm  - the plan7 model
 *                 
 * Return:   (void);
 *           the HMM probabilities are modified.
 */
static void
config_ls(struct plan7_s *hmm)
{
  hmm->xt[XTN][MOVE] = 1.-hmm->p1;    /* allow N-terminal tail */
  hmm->xt[XTN][LOOP] = hmm->p1;
  hmm->xt[XTE][MOVE] = 0.5;	      /* sets an expectation of 2 domains/seq, geometric dist */
  hmm->xt[XTE][LOOP] = 0.5;
  hmm->xt[XTC][MOVE] = 1.-hmm->p1;    /* allow C-terminal tail */
  hmm->xt[XTC][LOOP] = hmm->p1;
  hmm->xt[XTJ][MOVE] = 1.-hmm->p1;    /* allow J junction state */
  hmm->xt[XTJ][LOOP] = hmm->p1;

  hmm->nj    = 1.;		      /* because E->J = 0.5, geometric */

  FSet(hmm->begin+2, hmm->M-1, 0.);   /* disallow internal entries */
  hmm->begin[1]    = 1.0;
  FSet(hmm->end+1,   hmm->M-1, 0.);   /* disallow internal exits */
  hmm->end[hmm->M] = 1.0;

  left_wing_retraction_added(hmm);
  right_wing_retraction_added(hmm);
  normal_delete_scores(hmm);
  target_ldependence(hmm, 400);
  logoddsify_the_rest(hmm);

  hmm->mode   = P7_LS_MODE; 	 /* hmmls mode: glocal, multihit */
  hmm->flags &= ~PLAN7_BIMPOSED; /* no internal entry -> entry comes from wing retraction  */
  hmm->flags &= ~PLAN7_EIMPOSED; /* no internal exit  -> exit comes from wing retraction */
  hmm->flags |=  PLAN7_HASBITS;  /* we're configured */
}  
                             

/* config_sw()
 * 
 * Purpose:  Set the alignment independent parameters of
 *           a Plan7 model to hmmsw (Smith/Waterman) configuration.
 *           
 * Notes:    The desideratum for begin/end probs is that all fragments ij
 *           (starting at match i, ending at match j) are
 *           equiprobable -- there is no information in the choice of
 *           entry/exit. There are M(M+1)/2 possible choices of ij, so
 *           each must get a probability of 2/M(M+1). This prob is the
 *           product of a begin, an end, and all the not-end probs in
 *           the path between i,j. 
 *            
 *           Thus: entry/exit is asymmetric because of the left/right
 *           nature of the HMM/profile. Entry probability is distributed
 *           simply by assigning p_x = pentry / (M-1) to M-1 
 *           internal match states. However, the same approach doesn't
 *           lead to a flat distribution over exit points. Exit p's
 *           must be corrected for the probability of a previous exit
 *           from the model. Requiring a flat distribution over exit
 *           points leads to an easily solved piece of algebra, giving:
 *                      p_1 = pexit / (M-1)
 *                      p_x = p_1 / (1 - (x-1) p_1)
 *           
 * Args:     hmm    - the Plan7 model w/ data-dep prob's valid
 *           pentry - probability of an internal entry somewhere;
 *                    will be evenly distributed over M-1 match states
 *           pexit  - probability of an internal exit somewhere; 
 *                    will be distributed over M-1 match states.
 *                    
 * Return:   (void)
 *           HMM probabilities are modified.
 */
static void
config_sw(struct plan7_s *hmm)
{
  int   k;			/* counter over states      */

  /* Configure special states.
   */
  hmm->xt[XTN][MOVE] = 1-hmm->p1;    /* allow N-terminal tail */
  hmm->xt[XTN][LOOP] = hmm->p1;
  hmm->xt[XTE][MOVE] = 1.;	     /* disallow jump state   */
  hmm->xt[XTE][LOOP] = 0.;
  hmm->xt[XTC][MOVE] = 1-hmm->p1;    /* allow C-terminal tail */
  hmm->xt[XTC][LOOP] = hmm->p1;
  hmm->xt[XTJ][MOVE] = 1.;           /* J is unused */
  hmm->xt[XTJ][LOOP] = 0.;

  hmm->nj    = 0.;

  /* Configure entry:   (M-k+1) / [M(M+1)/2]   (xref STL9/77)
   * (tbd1 is ignored)
   */
  for (k = 1; k <= hmm->M; k++)
    hmm->begin[k] = 2. * (float) (hmm->M-k+1) / (float) hmm->M / (float) (hmm->M+1);

  /* Configure exit:   1/(M-k+1)  (xref STL9/77)
   */
  for (k = 1; k <= hmm->M; k++)
    hmm->end[k] = 1. / (float) (hmm->M-k+1);

  left_wing_retraction_imposed(hmm);
  right_wing_retraction_imposed(hmm);
  local_dpath_accounting(hmm);
  target_ldependence(hmm, 400);
  logoddsify_the_rest(hmm);

  hmm->mode   = P7_SW_MODE; 	/* hmmsw mode: local, onehit */
  hmm->flags |= PLAN7_BIMPOSED; /* internal entry set by algorithm mode */
  hmm->flags |= PLAN7_EIMPOSED; /* internal exit set by algorithm mode  */
  hmm->flags |= PLAN7_HASBITS;  /* we're configured */
}

/* config_fs()
 * Date:     SRE, Fri Jan  2 15:34:40 1998 [StL]
 * 
 * Purpose:  Set the alignment independent parameters of
 *           a Plan7 model to hmmfs (multihit Smith/Waterman) configuration.
 *           
 *           See comments on Plan7SWConfig() for explanation of
 *           how pentry and pexit are used.
 *           
 * Args:     hmm    - the Plan7 model w/ data-dep prob's valid
 *           pentry - probability of an internal entry somewhere;
 *                    will be evenly distributed over M-1 match states
 *           pexit  - probability of an internal exit somewhere; 
 *                    will be distributed over M-1 match states.
 *                    
 * Return:   (void)
 *           HMM probabilities are modified.
 */
static void
config_fs(struct plan7_s *hmm)
{
  int   k;			/* counter over states      */

  /* Configure special states.
   */
  hmm->xt[XTN][MOVE] = 1-hmm->p1;    /* allow N-terminal tail     */
  hmm->xt[XTN][LOOP] = hmm->p1;
  hmm->xt[XTE][MOVE] = 0.5;	     /* allow loops / multihits   */
  hmm->xt[XTE][LOOP] = 0.5;
  hmm->xt[XTC][MOVE] = 1-hmm->p1;    /* allow C-terminal tail     */
  hmm->xt[XTC][LOOP] = hmm->p1;
  hmm->xt[XTJ][MOVE] = 1.-hmm->p1;   /* allow J junction between domains */
  hmm->xt[XTJ][LOOP] = hmm->p1;

  hmm->nj = 1.;			/* E->J = 0.5 sets geometric distribution on J usage */

  /* Configure entry:   (M-k+1) / [M(M+1)/2]   (xref STL9/77)
   * (tbd1 is ignored)
   */
  for (k = 1; k <= hmm->M; k++)
    hmm->begin[k] = 2. * (float) (hmm->M-k+1) / (float) hmm->M / (float) (hmm->M+1);

  /* Configure exit:   1/(M-k+1)  (xref STL9/77)
   */
  for (k = 1; k <= hmm->M; k++)
    hmm->end[k] = 1. / (float) (hmm->M-k+1);

  left_wing_retraction_imposed(hmm);
  right_wing_retraction_imposed(hmm);
  local_dpath_accounting(hmm);
  target_ldependence(hmm, 400);
  logoddsify_the_rest(hmm);

  hmm->mode   = P7_FS_MODE; 	/* hmmfs mode: local, multihit */
  hmm->flags |= PLAN7_BIMPOSED; /* internal entry set by algorithm mode */
  hmm->flags |= PLAN7_EIMPOSED; /* internal exit set by algorithm mode  */
  hmm->flags |= PLAN7_HASBITS;  /* we're configured */
}


/*****************************************************************
 * Section 2. Wing retraction routines.
 * 
 * _imposed() forms are for local alignment: the alignment algorithm
 *      has set the entry or exit probabilities to fixed values.
 *      
 * _added() forms are for global/glocal alignment: the alignment
 *      algorithm per se doesn't allow internal entry or exit, so
 *      the wings are folded into the entry/exit probs.
 *      
 * xref STL9/81.
 *****************************************************************/

/* left_wing_retraction_imposed()
 * 
 * Wing retraction, when the B->M_k entry distribution is imposed by
 * the algorithm (sw, fs modes). No calculation is needed in this
 * case. The D_1 state is simply removed from the model.
 * 
 * bsc[1..M] scores are set. 
 * 
 * xref STL8/91.
 */
static void
left_wing_retraction_imposed(struct plan7_s *hmm)
{
  int k;

  for (k = 1; k <= hmm->M; k++)
    hmm->bsc[k] = Prob2Score(hmm->begin[k], hmm->p1);

  /* Virtual removal of D_1 state; 
   * assure that its transitions are impossible 
   */
  if (hmm->M > 1) 
    {
      hmm->tsc[TDM][1] = -INFTY;
      hmm->tsc[TDD][1] = -INFTY;
    }
  return;
}

/* left_wing_retraction_added()
 * 
 * Wing retraction, where B->M_k entry distribution comes entirely
 * from retracted paths for k>1 (begin[1] = 1.0 from algorithm; ls
 * mode, for example).
 * 
 * Sets bsc[1..M] (the entry/begin scores), using the core
 * model and the algorithmic begin[] distribution.
 * 
 * xref STL9/81.
 */
static void
left_wing_retraction_added(struct plan7_s *hmm)
{
  int    k;
  float  cumul;
  float *bmk;		/* log B->D...D->M_k path probabilities, BMk */
  float  x;

  bmk = MallocOrDie(sizeof(float) * (hmm->M+1));

  /* Calculate the log B->M_k path probabilities; xref STL9/81
   */
  bmk[1] = log(1. - hmm->tbd1);
  cumul = log(hmm->tbd1);
  for (k = 2; k <= hmm->M; k++)
    {
      bmk[k] = cumul + log(hmm->t[k-1][TDM]);
      cumul  += log(hmm->t[k-1][TDD]);
    }

  /* Renormalization (has little if any effect)
   * 
   * <cumul> now contains the log P of the B->D_1...D_M->E mute path
   * that we're removing. If (1-BE) is significantly different than
   * 1.0, renormalize the B distribution by dividing by (1-BE).  
   * Because log(1-x) = -x for x << 1, we know that subtracting
   * log(1-BE) from a log prob is only significant if logBE > log epsilon.
   */
  if (cumul > log(FLT_EPSILON))
    { 
      x = log(1. - exp(cumul));
      for (k = 1; k <= hmm->M; k++)
	bmk[k] -= x;
    }

  /* Conversion to scores. 
   * At this step, we're assuming that hmm->begin[k] = 0.0 for
   * k>1: the algorithm has no internal entries of its own, and
   * internal entry comes exclusively from paths through D states.
   */
  for (k = 1; k <= hmm->M; k++)
     hmm->bsc[k] = LL2Score(bmk[k], hmm->p1);
  
  /* Virtual removal of D_1 state.
   */
  if (hmm->M > 1)
    {
      hmm->tsc[TDM][1] = -INFTY;
      hmm->tsc[TDD][1] = -INFTY;
    }
  free(bmk);
  return;
}


/* right_wing_retraction_imposed()
 * 
 * Wing retraction for exits, for algorithms where M_k->E exit
 * probabilities are imposed by the algorithm (sw, fs modes). 
 * 
 * Sets esc[1..M] (the exit scores); also sets tsc[TM*][1..M-1], which
 * are affected by the presence of the new M_k->E probabilities.
 * 
 * xref STL9/81.
 */
static void
right_wing_retraction_imposed(struct plan7_s *hmm)
{
  int    k;
  float *mke;
  float  cumul;
  float  x;			/* temporary log prob */

  mke = MallocOrDie(sizeof(float) * (hmm->M+1));  

  /* The log prob of the wing-retracted M_k -> D...D -> E paths,
   * for k < M. (undefined for k == M).
   */
  cumul = 0.;
  for (k = hmm->M-1; k >= 1; k--)
    {
      mke[k] = cumul + log(hmm->t[k][TMD]);
      cumul += log(hmm->t[k][TDD]);
    }

  /* Set the esc[] and tsc[][TM*] scores.
   * 
   * The MkE path probability is subtracted from t[k][TMD] transition;
   * the match transitions are renormalized to account for the new
   * end[k] probability; and the match transitions are also renormalized
   * to account for the newly missing MkE path probability from TMD.
   * (xref STL9/91 for details).
   */
  for (k = 1; k < hmm->M; k++)
    {
      hmm->esc[k] = Prob2Score(hmm->end[k], 1.0); /* end[k] is imposed. */

      x = log(hmm->t[k][TMM]);
      if (hmm->end[k] > FLT_EPSILON) x += log(1. - hmm->end[k]);
      if (mke[k] > log(FLT_EPSILON)) x -= log(1. - exp(mke[k]));
      hmm->tsc[TMM][k] = LL2Score(x, hmm->p1);

      x = log(hmm->t[k][TMI]);
      if (hmm->end[k] > FLT_EPSILON) x += log(1. - hmm->end[k]);
      if (mke[k] > log(FLT_EPSILON)) x -= log(1. - exp(mke[k]));
      hmm->tsc[TMI][k] = LL2Score(x, hmm->p1);

      x = log(hmm->t[k][TMD]);
      if (mke[k] - x > log(FLT_EPSILON)) x += log(1. - exp(mke[k] - x));
      if (hmm->end[k] > FLT_EPSILON)     x += log(1. - hmm->end[k]);         
      if (mke[k] > log(FLT_EPSILON))     x -= log(1. - exp(mke[k]));
      hmm->tsc[TMD][k] = LL2Score(x, 1.0);
    }
  hmm->esc[hmm->M] = 0.0;	/* by definition */

  /* Note that node M isn't even explicitly represented in the
   * configured HMM scores -- tsc[][] only contains values for
   * 1..M-1. So there's no need to set any M_M or D_M transition
   * scores to 0 and -INFTY as we virtually remove D_M state;
   * the only other affected score is tsc[TDM][hmm->M-1], which
   * is going to be set when we deal with delete paths.
   * So, we're done.
   */
  free(mke);
  return;
}


/* right_wing_retraction_added()
 * 
 * Retract the right wing (remove the D_M state, and all paths through
 * it), for algorithms which have no M_k->E end[k] internal exit
 * probability. The Mk->Dk+1...DM->E path probabilities are therefore
 * subtracted from t[k][TMD] and added to end[k].
 *
 * Sets esc[1..M] (the exit scores); also sets tsc[TM*][1..M-1], which
 * are affected by the presence of the new M_k->E probabilities.
 * 
 * xref STL9/81.
 */
static void
right_wing_retraction_added(struct plan7_s *hmm)
{
  int    k;
  float *mke;
  float  cumul;
  float  x;			/* temporary log prob */

  mke = MallocOrDie(sizeof(float) * (hmm->M+1));  

  /* The log prob of the wing-retracted M_k -> D...D -> E paths,
   * for k < M. (undefined for k == M).
   */
  cumul = 0.;
  for (k = hmm->M-1; k >= 1; k--)
    {
      mke[k] = cumul + log(hmm->t[k][TMD]);
      cumul += log(hmm->t[k][TDD]);
    }

  /* Set the esc[] and tsc[TM*][] scores.
   * 
   * The end probability is assumed to be exclusively the MkE
   * path probability; algorithm has no internal exit prob of its own.
   * 
   * The MkE path probability is moved from the t[k][TMD] transition
   * to the end[k] probability. No renormalization is needed, because
   * prob is conserved: we assume that the algorithm added no
   * internal exit probability end[k] of its own.
   * (xref STL9/91 for details).
   */
  for (k = 1; k < hmm->M; k++)
    {
      hmm->esc[k] = LL2Score(mke[k], 1.0); /* M->E comes only thru terminal deletes */

      hmm->tsc[TMM][k] = Prob2Score(hmm->t[k][TMM], hmm->p1);
      hmm->tsc[TMI][k] = Prob2Score(hmm->t[k][TMI], hmm->p1);
      
      x = log(hmm->t[k][TMD]);
      if ((mke[k] - x) > log(FLT_EPSILON)) x += log(1. - exp(mke[k] - x));
      hmm->tsc[TMD][k] = LL2Score(x, 1.0);
    }
  hmm->esc[hmm->M] = 0.0;	/* by definition */

  /* Note that node M isn't even explicitly represented in the
   * configured HMM scores -- tsc[][] only contains values for
   * 1..M-1. So there's no need to set any M_M or D_M transition
   * scores to 0 and -INFTY as we virtually remove D_M state;
   * the only other affected score is tsc[TDM][hmm->M-1], which
   * is going to be set when we deal with delete paths.
   * So, we're done.
   */
  free(mke);
  return;
}


/*****************************************************************
 * Section 3. Local alignment D-path accounting
 *
 * Fully local alignment modes that are trying to make all fragments
 * ij equiprobable use "dpath accounting": they add a score penalty
 * of log(1-e_k) onto every D->D and D->M transition. 
 *
 * These routines are responsible for setting all tsc[TD*] scores,
 * from k = 2..M-1.
 *
 * State D_1 was already "removed", and both TDD and TDM transitions
 * set to -INFTY, in left wing retraction.
 * 
 * State D_M has only been partly removed though. The M_k-1 -> D_M
 * transition was set to -INFTY by right wing retraction. D_M itself
 * has no transitions in the model, since t[] only runs 1..M-1. 
 * But we still need to make sure t'[k-1][TDM] = 1.0 and 
 * t'[k-1][TDD] = 0.0 in the configured model. Routines 
 * for d-path accounting are responsible for this.
 *****************************************************************
 */

static void
local_dpath_accounting(struct plan7_s *hmm)
{
  int   k;
  float x;

  for (k = 2; k < hmm->M-1; k++)
    {
      x = log(hmm->t[k][TDM]);
      if (hmm->end[k] > FLT_EPSILON) x += log(1.0 - hmm->end[k]);
      hmm->tsc[TDM][k] = LL2Score(x, hmm->p1);

      x = log(hmm->t[k][TDD]);
      if (hmm->end[k] > FLT_EPSILON) x += log(1.0 - hmm->end[k]);
      hmm->tsc[TDD][k] = LL2Score(x, 1.0);
    }
  /* Handle M-1 differently, since D_M is being removed.
   */
  if (hmm->M > 1)
    {
      x = 0.0;
      if (hmm->end[hmm->M-1] > FLT_EPSILON) x += log(1.0 - hmm->end[hmm->M-1]);
      hmm->tsc[TDM][hmm->M-1] = LL2Score(x, hmm->p1);
      hmm->tsc[TDD][hmm->M-1] = -INFTY;
    }
}

static void
normal_delete_scores(struct plan7_s *hmm)
{
  int k;

  for (k = 2; k < hmm->M-1; k++)
    {
      hmm->tsc[TDM][k] = Prob2Score(hmm->t[k][TDM], hmm->p1);
      hmm->tsc[TDD][k] = Prob2Score(hmm->t[k][TDD], 1.0);
    }
  /* Handle M-1 differently, since D_M is being removed.
   */
  if (hmm->M > 1)
    {
      hmm->tsc[TDM][hmm->M-1] = Prob2Score(1.0, hmm->p1);
      hmm->tsc[TDD][hmm->M-1] = -INFTY;
    }
}


/*****************************************************************
 * Section 4. Target length dependence.
 * 
 * Here we set the p1 and xt[NCJ] probabilities, and the
 * xt[NCJ] scores, which control target length dependence of the
 * model. 
 * 
 * A special numerical issue (STL10/26): we will have trouble with
 * the [NCJ][LOOP] scores for large L, because these scores will
 * be ~ 1/L, and we can only hold scores down to 0.001 bits if 
 * INTSCALE is at its default 1000. As a workaround, we catch the
 * case where the absolute value of one of these scores would be <10;
 * in this case, we set a PLAN7_LCORRECT flag in the hmm, and store
 * the score in floating pt as hmm->lscore. We can then post-hoc
 * correct an alignment score by (L-kappa) * hmm->lscore. 
 *****************************************************************/

static void
target_ldependence(struct plan7_s *hmm, int L)
{
  double Lp;		/* target length that aligns to non-model states NCJ */
  double ploop, pmove;

  /* Configure hmm->p1 in the null model to an expected length of L
   */
  hmm->p1 = (double) L / (double) (L+1);
  
  /* For small L/large kappa, we have to avoid calculating a negative
   * probability below, which means not making Lp negative. Totally
   * arbitrarily, enforce a minimum of 1 on Lp.
   */
  Lp = (double) L - hmm->kappa;
  if (Lp < 1.) Lp = 1.;

  /* Configure N,J,C transitions so they bear Lp/(1+n) of the load of
   * generating Lp. 
   */
  pmove = (2. + hmm->nj) / (Lp + 2. + hmm->nj); /* 2/(Lp+2) for sw; 3/(Lp+3) for fs */
  ploop = 1. - pmove;
  hmm->xt[XTN][MOVE] = pmove;
  hmm->xt[XTN][LOOP] = ploop;
  hmm->xt[XTC][MOVE] = pmove;
  hmm->xt[XTC][LOOP] = ploop;
  hmm->xt[XTJ][MOVE] = pmove;
  hmm->xt[XTJ][LOOP] = ploop;

  /* Set the scores.
   */
  hmm->xsc[XTN][LOOP] = Prob2Score(hmm->xt[XTN][LOOP], hmm->p1);
  hmm->xsc[XTN][MOVE] = Prob2Score(hmm->xt[XTN][MOVE], 1.0);
  hmm->xsc[XTC][LOOP] = Prob2Score(hmm->xt[XTC][LOOP], hmm->p1);
  hmm->xsc[XTC][MOVE] = Prob2Score(hmm->xt[XTC][MOVE], 1.-hmm->p1);
  hmm->xsc[XTJ][LOOP] = Prob2Score(hmm->xt[XTJ][LOOP], hmm->p1);
  hmm->xsc[XTJ][MOVE] = Prob2Score(hmm->xt[XTJ][MOVE], 1.0);

  /* Detect the "special" (actually common) case of the LOOP scores
   * being too small to keep track of properly. This code assumes that
   * all three scores (N,C,J)[LOOP] are equal, which is how we set them
   * above.
   */
  if (abs(hmm->xsc[XTN][LOOP]) < 10) 
    {
      hmm->xsc[XTN][LOOP] = 0;
      hmm->xsc[XTC][LOOP] = 0;
      hmm->xsc[XTJ][LOOP] = 0;
      hmm->flags |= PLAN7_LCORRECT;
      hmm->lscore = 1.4427 * log(hmm->xt[XTN][LOOP] / hmm->p1);
    }
  else
    hmm->flags &= ~PLAN7_LCORRECT;
}


/*****************************************************************
 * Section 5. Conversion of everything else to log-odds scores.
 * 
 * The log-odds scores account for null model as follows:          
 *         type of parameter       probability        score
 *         -----------------       -----------        ------
 *         any emission             p_x           log_2 p_x/null_x
 *             N,J,C /assume/ p_x = null_x so /always/ score zero.  
 *         transition to emitters   t_x           log_2 t_x/p1
 *            (M,I; N,C; J)
 *             NN and CC loops are often equal to p1, so usu. score zero.
 *         C->T transition          t_x            log_2 t_x/p2 
 *            often zero, usu. C->T = p2. 
 *         all other transitions    t_x           log_2 t_x
 *             (no null model counterpart, so null prob is 1)    
 * 
 * bsc[] have already been set to scores by left wing retraction.
 * tsc[TM*], esc[] were set by right wing retraction.
 * tsc[TD*] were set by delete path accounting.
 * 
 * We're responsible for tsc[TI*], msc[], isc[], and xsc[]. 
 ******************************************************************/  

static void
logoddsify_the_rest(struct plan7_s *hmm)
{
  int k;			/* counter for model position */
  int x;			/* counter for symbols        */

  /* Symbol emission scores, match msc[] and insert isc[].
   */
  for (k = 1; k <= hmm->M; k++) 
    {
				/* match/insert emissions in main model */
      for (x = 0; x < Alphabet_size; x++) 
	{
	  hmm->msc[x][k] = Prob2Score(hmm->mat[k][x], hmm->null[x]);
	  if (k < hmm->M) 
	    hmm->isc[x][k] =  Prob2Score(hmm->ins[k][x], hmm->null[x]); 
	}
				/* degenerate match/insert emissions */
      for (x = Alphabet_size; x < Alphabet_iupac; x++) 
	{
	  hmm->msc[x][k] = DegenerateSymbolScore(hmm->mat[k], hmm->null, x);
	  if (k < hmm->M)
	    hmm->isc[x][k] = DegenerateSymbolScore(hmm->ins[k], hmm->null, x);
	}
    }

  /* The tsc[TI*] insert transition scores.
   */
  for (k = 1; k < hmm->M; k++)
    {
      hmm->tsc[TIM][k] = Prob2Score(hmm->t[k][TIM], hmm->p1);
      hmm->tsc[TII][k] = Prob2Score(hmm->t[k][TII], hmm->p1);
    }

  /* The xsc[XTE] special state scores.
   * N,C,J were already set by target_ldependence().
   */
  hmm->xsc[XTE][LOOP] = Prob2Score(hmm->xt[XTE][LOOP], 1.0);
  hmm->xsc[XTE][MOVE] = Prob2Score(hmm->xt[XTE][MOVE], 1.0);
  return;
}





/************************************************************
 * @LICENSE@
 ************************************************************/