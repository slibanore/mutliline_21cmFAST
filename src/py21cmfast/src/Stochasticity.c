/*functions which deal with stochasticity
 * i.e sampling the halo mass function and
 * other halo relations.*/

//BIG TODO: sort out single/double precision all the way through
//STYLE: make the function names consistent, re-order functions
//so it makes sense and make a map of the call trees to look for modularisation

//max guesses for rejection sampling / root finding
//it should be low because both should take <20 attempts
#define MAX_ITERATIONS 1e3

//max halos in memory for test functions
//buffer size (per cell of arbitrary size) in the sampling function
//this is big enough for a ~20Mpc mean density cell
//TODO: both of these should be set depending on size, resolution & min mass
#define MAX_HALO_CELL (int)1e5
//Max halo in entire box
#define MAX_HALO (int)1e9
//per halo in update
#define MAX_HALO_UPDATE 1024

#define MMAX_TABLES 1e20
#define MANY_HALOS 100 //enough halos that we don't care about stochasticity, for future acceleration

#define N_MASS_INTERP (int)100
#define N_DELTA_INTERP (int)100
#define N_PROB (int)200
#define MIN_LOGPROB (double)-20. //exp(-20) ~ 2e-9, minimum probability P(>M) for tables
#define RF_CONV 1e-3 //Convergence for root finder, units of ln(prob), MUST BE LESS THAN -MIN_LOGPROB/N_PROB

struct AstroParams *astro_params_stoc;
struct CosmoParams *cosmo_params_stoc;
struct UserParams *user_params_stoc;
struct FlagOptions *flag_options_stoc;

struct parameters_gsl_MF_con_int_{
    double redshift;
    double growthf;
    double delta;
    double n_order;
    double sigma_max;
    double M_max;
    int HMF;
};

//Modularisation that should be put in ps.c for the evaluation of sigma
double EvaluateSigma(double lnM){
    //using log units to make the fast option faster and the slow option slower
    double sigma;
    float MassBinLow;
    int MassBin;

    //all this stuff is defined in ps.c and initialised with InitialiseSigmaInterpTable
    if(user_params_ps->USE_INTERPOLATION_TABLES) {
        MassBin = (int)floor( (lnM - MinMass )*inv_mass_bin_width );
        MassBinLow = MinMass + mass_bin_width*(float)MassBin;

        sigma = Sigma_InterpTable[MassBin] + ( lnM - MassBinLow )*( Sigma_InterpTable[MassBin+1] - Sigma_InterpTable[MassBin] )*inv_mass_bin_width;
    }
    else {
        sigma = sigma_z0(exp(lnM));
    }

    return sigma;
}

/*
 copied from ps.c but with interpolation tables
 TODO: put this and EvaluateSigma in ps.c
 */
double EvaluateFgtrM(double z, double lnM, double del_bias, double sig_bias){
    double del, sig, sigsmallR;

    //LOG_ULTRA_DEBUG("FgtrM: z=%.2f M=%.3e d=%.3e s=%.3e",z,exp(lnM),del_bias,sig_bias);

    sigsmallR = EvaluateSigma(lnM);

    //LOG_ULTRA_DEBUG("FgtrM: SigmaM %.3e",sigsmallR);

    del = Deltac/dicke(z) - del_bias;

    if(del < 0){
        LOG_ERROR("error in FgtrM: condition sigma %.3e delta %.3e arg sigma %.3e delta %.3e",sig_bias,del_bias,sigsmallR,Deltac/dicke(z));
        Throw(ValueError);
    }
    //sometimes condition mass is close enough to minimum mass such that the sigmas are the same to float precision
    //In this case we just throw away the halo, since it is very unlikely to have progenitors
    if(sigsmallR <= sig_bias){
        return 0.;
    }

    sig = sqrt(sigsmallR*sigsmallR - sig_bias*sig_bias);

    return splined_erfc(del / (sqrt(2)*sig));
}

void Broadcast_struct_global_STOC(struct UserParams *user_params, struct CosmoParams *cosmo_params,struct AstroParams *astro_params, struct FlagOption *flag_options){
    cosmo_params_stoc = cosmo_params;
    user_params_stoc = user_params;
    astro_params_stoc = astro_params;
    flag_options_stoc = flag_options;
}

// TODO: this should probably be in UsefulFunctions.c
void seed_rng_threads(gsl_rng * rng_arr[], int seed){
    // setting tbe random seeds (copied from GenerateICs.c)
    gsl_rng * rseed = gsl_rng_alloc(gsl_rng_mt19937); // An RNG for generating seeds for multithreading

    gsl_rng_set(rseed, seed);

    unsigned int seeds[user_params_stoc->N_THREADS];

    // For multithreading, seeds for the RNGs are generated from an initial RNG (based on the input random_seed) and then shuffled (Author: Fred Davies)
    int num_int = INT_MAX/16;
    int i, thread_num;
    unsigned int *many_ints = (unsigned int *)malloc((size_t)(num_int*sizeof(unsigned int))); // Some large number of possible integers
    for (i=0; i<num_int; i++) {
        many_ints[i] = i;
    }

    gsl_ran_choose(rseed, seeds, user_params_stoc->N_THREADS, many_ints, num_int, sizeof(unsigned int)); // Populate the seeds array from the large list of integers
    gsl_ran_shuffle(rseed, seeds, user_params_stoc->N_THREADS, sizeof(unsigned int)); // Shuffle the randomly selected integers

    int checker;

    checker = 0;
    // seed the random number generators
    for (thread_num = 0; thread_num < user_params_stoc->N_THREADS; thread_num++){
        switch (checker){
            case 0:
                rng_arr[thread_num] = gsl_rng_alloc(gsl_rng_mt19937);
                gsl_rng_set(rng_arr[thread_num], seeds[thread_num]);
                break;
            case 1:
                rng_arr[thread_num] = gsl_rng_alloc(gsl_rng_gfsr4);
                gsl_rng_set(rng_arr[thread_num], seeds[thread_num]);
                break;
            case 2:
                rng_arr[thread_num] = gsl_rng_alloc(gsl_rng_cmrg);
                gsl_rng_set(rng_arr[thread_num], seeds[thread_num]);
                break;
            case 3:
                rng_arr[thread_num] = gsl_rng_alloc(gsl_rng_mrg);
                gsl_rng_set(rng_arr[thread_num], seeds[thread_num]);
                break;
            case 4:
                rng_arr[thread_num] = gsl_rng_alloc(gsl_rng_taus2);
                gsl_rng_set(rng_arr[thread_num], seeds[thread_num]);
                break;
        } // end switch

        checker += 1;

        if(checker==5) {
            checker = 0;
        }
    }

    gsl_rng_free(rseed);
    free(many_ints);
}

void free_rng_threads(gsl_rng * rng_arr[]){
    int ii;
    for(ii=0;ii<user_params_stoc->N_THREADS;ii++){
        gsl_rng_free(rng_arr[ii]);
    }
}

//n_order is here because the variance calc can use these functions too
//remove the variable (n==0) if we remove that calculation
double MnMassfunction(double M, void *param_struct){
    struct parameters_gsl_MF_con_int_ params = *(struct parameters_gsl_MF_con_int_ *)param_struct;
    double mf;
    double growthf = params.growthf;
    double delta = params.delta;
    double n_order = params.n_order;
    double sigma2 = params.sigma_max; //M2 and sigma2 are degenerate, remove one
    double M_filter = params.M_max;
    double z = params.redshift;
    //HMF = user_params.HMF unless we want the conditional in which case its -1
    int HMF = params.HMF;

    double M_exp = exp(M);

    //M1 is the mass of interest, M2 doesn't seem to be used (input as max mass),
    // delta1 is critical, delta2 is current, sigma is sigma(Mmax,z=0)
    //WE WANT DNDLOGM HERE, SO WE ADJUST ACCORDINGLY

    //dNdlnM = dfcoll/dM * M / M * constants
    //All unconditional functions are dNdM, conditional is actually dfcoll dM == dNdlogm * constants
    if(HMF==0) {
        mf = dNdM(growthf, M_exp) * M_exp;
    }
    else if(HMF==1) {
        mf = dNdM_st(growthf, M_exp) * M_exp;
    }
    else if(HMF==2) {
        mf = dNdM_WatsonFOF(growthf, M_exp) * M_exp;
    }
    else if(HMF==3) {
        mf = dNdM_WatsonFOF_z(z, growthf, M_exp) * M_exp;
    }
    else if(HMF==-1) {
        if (M_filter < M) mf = 0;
        else mf = dNdM_conditional(growthf,M,M_filter,Deltac,delta,sigma2);
    }
    else {
        return -1;
    }
    //norder for expectation values of M^n
    return exp(M * (n_order)) * mf;
}

//copied mostly from the Nion functions
//I might be missing something like this that already exists somewhere in the code
//TODO: rename since its now an integral of M * dfcoll/dm = dNdM / [(RHOcrit * (1+delta) / sqrt(2.*PI) * cosmo_params_stoc->OMm)]
double IntegratedNdM(double growthf, double M1, double M2, double M_filter, double delta, double n_order, int HMF){
    double result, error, lower_limit, upper_limit;
    gsl_function F;
    double rel_tol = 0.01; //<- relative tolerance
    gsl_integration_workspace * w
    = gsl_integration_workspace_alloc (1000);

    double sigma = EvaluateSigma(M_filter);

    struct parameters_gsl_MF_con_int_ parameters_gsl_MF_con = {
        .growthf = growthf,
        .delta = delta,
        .n_order = n_order,
        .sigma_max = sigma,
        .M_max = M_filter,
        .HMF = HMF,
    };
    int status;

    F.function = &MnMassfunction;
    F.params = &parameters_gsl_MF_con;
    lower_limit = M1;
    upper_limit = M2;

    gsl_set_error_handler_off();

    status = gsl_integration_qag (&F, lower_limit, upper_limit, 0, rel_tol,
                         1000, GSL_INTEG_GAUSS61, w, &result, &error);

    if(status!=0) {
        LOG_ERROR("gsl integration error occured!");
        LOG_ERROR("(function argument): lower_limit=%.3e upper_limit=%.3e rel_tol=%.3e result=%.3e error=%.3e",lower_limit,upper_limit,rel_tol,result,error);
        LOG_ERROR("data: growthf=%.3e M2=%.3e delta=%.3e sigma2=%.3e HMF=%.3d order=%.3e",growthf,M_filter,delta,sigma,HMF,n_order);
        //LOG_ERROR("data: growthf=%e M2=%e delta=%e,sigma2=%e",parameters_gsl_MF_con.growthf,parameters_gsl_MF_con.M_max,parameters_gsl_MF_con.delta,parameters_gsl_MF_con.sigma_max);
        GSL_ERROR(status);
    }

    gsl_integration_workspace_free (w);

    return result;
}

gsl_spline *Nhalo_spline;
gsl_interp_accel *Nhalo_acc;
#pragma omp threadprivate(Nhalo_acc)

double EvaluatedNdMSpline(double x_in){
    double result = gsl_spline_eval(Nhalo_spline, x_in, Nhalo_acc);
    return result;
}

void initialise_dNdM_table(double xmin, double xmax, double growth1, double param_2, double M_min, bool update){
    double x, buf;
    int nx;
    int gsl_status;
    //halo catalogue update, x=M_filter, delta = Deltac/D1*D2, param2 = growth2
    if(update){
        nx = N_MASS_INTERP;
    }
    //generation from grid, x=delta, param_2 = log(M_filt)
    else{
        nx = N_DELTA_INTERP;
    }

    double ya[nx], xa[nx];
    int i;

    //set up coordinate grids
    for(i=0;i<nx;i++) xa[i] = xmin + (xmax - xmin)*(i)/(nx-1);

    if(update)
        LOG_DEBUG("Initialising dNdM Table from %.2e to %.2e, D_out %.2e D_in %.2e min %.2e up %d",exp(xmin),exp(xmax),growth1,param_2,exp(M_min),update);
    else
        LOG_DEBUG("Initialising dNdM Table from %.2e to %.2e, D %.2e M_max %.2e M_min %.2e up %d",xmin,xmax,growth1,exp(param_2),exp(M_min),update);

    //this should work generally, but there MAY be faster ways that are more specific to each HMF
    //if they are integrable analytically
    for(i=0;i<nx;i++){
        x = xa[i];
        if(update){
            if(x <= M_min){
                 buf = 0;
                 continue;
            }
            buf = IntegratedNdM(growth1, M_min, x, x, Deltac*growth1/param_2, 0, -1);
        }
        else{
            if(x >= Deltac || x <= -1){
                buf = 0; //>Deltac is handled outside the tables
                continue;
            }
            buf = IntegratedNdM(growth1, M_min, param_2, param_2, x, 0, -1);
        }
        ya[i] = buf;
    }

    //initialise and fill the interp table
    //The accelerators in GSL interpolators are not threadsafe, so we need one per thread.
    //Since it's not super important which thread has which accelerator, just that they
    //aren't using the same one at the same time, I think this is safe
#pragma omp parallel num_threads(user_params_stoc->N_THREADS)
    {
        Nhalo_acc = gsl_interp_accel_alloc();
    }
    Nhalo_spline  = gsl_spline_alloc(gsl_interp_linear, nx);
    gsl_status = gsl_spline_init(Nhalo_spline, xa, ya, nx);
    GSL_ERROR(gsl_status);

/*#if LOG_LEVEL >= ULTRA_DEBUG_LEVEL

    for(i=0;i<nx;i++){
        x = xa[i];
        buf = gsl_spline_eval(Nhalo_spline,x,Nhalo_acc);
        LOG_ULTRA_DEBUG("Nhalo at x=%.2e = %.2e |interp: %.2e",x,ya[i],buf);
    }    
#endif*/
    return;
}

void free_dNdM_table(){
    gsl_spline_free(Nhalo_spline);

    #pragma omp parallel num_threads(user_params_stoc->N_THREADS)
    {
        gsl_interp_accel_free(Nhalo_acc);
    }
    return;
}

//2D interpolation tables for the INVERSE CDF of halo masses
//TODO: each time stoc_halo_sample is called, only one row(x) is used
//which means there is likely a way to accelerate more

gsl_spline2d *inv_NgtrM_spline;
gsl_interp_accel *inv_NgtrM_arg_acc;
#pragma omp threadprivate(inv_NgtrM_arg_acc)
gsl_interp_accel *inv_NgtrM_prob_acc;
#pragma omp threadprivate(inv_NgtrM_prob_acc)

void initialise_inverse_table(double xmin, double xmax, double zmin, double growth1, double growth2, bool update){
    
    double lnMfilt=0.,delta_in=0.;
    int nx, ny;
    double sigma, Mfilt;
    int i,j,k;

    ny = N_PROB;
    if(update){
        //halo catalogue update, z=M_prog, x=M_filt, delta = Deltac/D1*D2
        nx = N_MASS_INTERP;
        delta_in = Deltac*growth1/growth2;
    }
    else{
        //generation from grid, z=M_halo, x=delta, M_filt = RtoM(R)*(1+delta)
        nx = N_DELTA_INTERP;
        Mfilt = RtoM(user_params_stoc->BOX_LEN / user_params_stoc->HII_DIM * L_FACTOR);
        lnMfilt = log(Mfilt);
        sigma = EvaluateSigma(lnMfilt);
    }
    double za[nx*ny], xa[nx], ya[ny];

    LOG_DEBUG("Initialising Inverse Table x [%.2e,%.2e] z[%.2e...] D_1 %.2e D_2 %.2e, M %.2e, up %d", update ? exp(xmin) : xmin
                ,update ? exp(xmax) : xmax, exp(zmin), growth1, growth2, Mfilt, update);

    //set up coordinate grids
    for(i=0;i<nx;i++) xa[i] = xmin + (xmax - xmin)*i/(nx-1);
    for(j=0;j<ny;j++) ya[ny-j-1] = MIN_LOGPROB*(double)j/(ny-1); //y = log(prob > M)

    //set up the spline object
    inv_NgtrM_spline = gsl_spline2d_alloc(gsl_interp2d_bilinear,nx,ny);

    //We need a table that goes from (condition,prob) -> Mass, we do this by root finding the integrated CMF to the desired probability
    //we can't simply transpose the (Mass,condition) -> prob table since the probabilities need to be a grid (same for all conditions)
#pragma omp parallel private(i,j,k) firstprivate(sigma,lnMfilt,delta_in)
    {
        double x, y, z;
        double z_init,z_low,z_high;
        double f_low,f_high;
        double buf,norm,f;

#pragma omp for
        for(i=0;i<nx;i++){
            
            x = xa[i];
            
            if(update){
                lnMfilt = x;
                sigma = EvaluateSigma(x);
            }
            else{
                delta_in = x;
            }
            z_init = (lnMfilt + zmin)/2; // 1/2 way point for initial guess

            //set minimum mass limits
            gsl_interp2d_set(inv_NgtrM_spline,za,i,0,zmin);
            
            //LOG_ULTRA_DEBUG("Integrating D=%.2f from %.3e, x=%.2e filt %.2e delta %.2e",growth1,zmin,x,lnMfilt,delta_in);
            norm = IntegratedNdM(growth1, zmin, lnMfilt, lnMfilt, delta_in, 0, -1);
            for(j=1;j<ny;j++){
                y = ya[j];

                //to prevent delta ~ Deltac issues
                if(!update && x>=Deltac){
                    gsl_interp2d_set(inv_NgtrM_spline,za,i,j,lnMfilt);
                    continue;
                }
                
                //if the condition is too small to host halos at the required redshift, we set to the minimum mass
                if(norm == 0){
                    gsl_interp2d_set(inv_NgtrM_spline,za,i,j,zmin);
                    continue;
                }
                //find the mass which makes y% of the halos
                z = z_init;
                z_low = zmin;
                z_high = lnMfilt;
                f_low = -y; //p <= 1
                f_high = 2*MIN_LOGPROB; //p > exp(MIN_LOGPROB), the filter mass will really be at -inf but setting this is reasonable
                //Simple linear guess root finding, dNdM has high curvature at higher masses so derivative methods are shaky
                //I should use GSL for this for consistency but that requires copypasting some functions
                for(k=0;k<MAX_ITERATIONS;k++){
                    //fraction of halos in condition
                    //LOG_ULTRA_DEBUG("Integrating D=%.2f from %.3e (%.3e) to %.3e delta %.2e filt %.2e.",growth1,exp(z),exp(z_init),exp(lnMfilt),delta_in,exp(lnMfilt));
                    buf = IntegratedNdM(growth1, z, lnMfilt, lnMfilt, delta_in, 0, -1) / norm;
                    
                    //sometimes close to the filter mass the probability goes to zero.
                    //Let's pretend the function flattens shortly after our minimum P, this shouldn't affect the root
                    //it will make the next guess an overestimate, but should still be faster than a bisection
                    if(buf < exp(2*MIN_LOGPROB)){
                        buf = exp(2*MIN_LOGPROB);
                    }

                    //check if root found
                    f = log(buf) - y;

                    if(f!=f){
                        LOG_ERROR("Nan reached in table construction x=%.6e y=%.6e z=%.6e f=%.3e, p=%.3e, lnp=%.3e",update ? exp(x) : x,y,exp(z),f,buf,log(buf));
                        Throw(TableGenerationError);
                    }

                    if(f>0){
                        z_low = z; //logp > y, mass too low
                        f_low = f; //note: not the low probability but the probability of the lower mass limit
                    }
                    if(f<0){
                        z_high = z; //logp < y, mass too high
                        f_high = f;
                    }

                    //LOG_ULTRA_DEBUG("Attempt %d || x=%.6e y=%.6e z=%.6e f=%.3e, p=%.3e, lnp=%.3e nextz %.3e",k,update ? exp(x) : x,y,exp(z),f,buf,log(buf), exp(z_low - f_low*(z_high - z_low)/(f_high - f_low)));
                    //LOG_ULTRA_DEBUG("z_low %.6e z_high %.6e f_low %.6e f_high %.6e",exp(z_low),exp(z_high),f_low,f_high);

                    z = z_low - f_low*(z_high - z_low)/(f_high - f_low); //draw line between your two limits and guess where 0 is
                    
                    if(fabs(f) < RF_CONV || z == z_low || z == z_high){
                        gsl_interp2d_set(inv_NgtrM_spline,za,i,j,z);
                        z_init = z; //the next mass will be close to this one
                        //LOG_ULTRA_DEBUG("found x=%.6e y=%.6e (%.6e) z=%.6e in %d attempts",update ? exp(x) : x,y,exp(y),exp(z),k+1);
                        break;
                    }
                }
                if(k==MAX_ITERATIONS){
                    LOG_ERROR("max iterations reached in table construction x=%.6e y=%.6e z=%.6e",update ? exp(x) : x,y,exp(z));
                    Throw(TableGenerationError);
                }
            }
        }

        inv_NgtrM_arg_acc = gsl_interp_accel_alloc();
        inv_NgtrM_prob_acc = gsl_interp_accel_alloc();
    }

    gsl_spline2d_init(inv_NgtrM_spline,xa,ya,za,nx,ny);
    
    return;
}

void free_inverse_table(){
    gsl_spline2d_free(inv_NgtrM_spline);

    #pragma omp parallel num_threads(user_params_stoc->N_THREADS)
    {
        gsl_interp_accel_free(inv_NgtrM_arg_acc);
        gsl_interp_accel_free(inv_NgtrM_prob_acc);
    }
    return;
}

//set the minimum source mass
//TODO: include MINI_HALOS
double minimum_source_mass(double redshift,struct AstroParams *astro_params, struct FlagOptions * flag_options){
    double Mmin;
    if(flag_options->USE_MASS_DEPENDENT_ZETA) {
        Mmin = astro_params->M_TURN / 50;
    }
    else {
        if(flag_options->M_MIN_in_Mass) {
            Mmin = astro_params->M_TURN / 50;
        }
        else {
            //set the minimum source mass
            if (astro_params->ION_Tvir_MIN < 9.99999e3) { // neutral IGM
                Mmin = TtoM(redshift, astro_params->ION_Tvir_MIN, 1.22);
            }
            else { // ionized IGM
                Mmin = TtoM(redshift, astro_params->ION_Tvir_MIN, 0.6);
            }
        }
    }
    return Mmin;
}

//This function adds stochastic halo properties to an existing halo
//TODO: this needs to be updated to handle MINI_HALOS and not USE_MASS_DEPENDENT_ZETA flags
int add_halo_properties(gsl_rng *rng, float halo_mass, float redshift, float * output){
    //for now, we just have stellar mass
    double f10 = astro_params_stoc->F_STAR10;
    double fa = astro_params_stoc->ALPHA_STAR;
    double sigma_star = astro_params_stoc->SIGMA_STAR;
    double sigma_sfr = astro_params_stoc->SIGMA_SFR;

    double fstar_mean, f_sample, sm_sample;
    double sfr_mean, sfr_sample;
    double dutycycle_term;

    //duty cycle, TODO: think about a way to explicitly include the binary nature consistently with the updates
    //At the moment, we simply reduce the mean
    dutycycle_term = exp(-astro_params_stoc->M_TURN/halo_mass);

    //This clipping is normally done with the mass_limit_bisection root find.
    //I can't do that here with the stochasticity, since the f_star clipping happens AFTER the sampling
    fstar_mean = f10 * pow(halo_mass/1e10,fa) * dutycycle_term;
    if(sigma_star > 0){
        //sample stellar masses from each halo mass assuming lognormal scatter
        f_sample = gsl_ran_ugaussian(rng);
        
        /* Simply adding lognormal scatter to a delta increases the mean (2* is as likely as 0.5*)
        * We multiply by exp(-sigma^2/2) so that X = exp(mu + N(0,1)*sigma) has the desired mean */
        f_sample = fmin(fstar_mean * exp(-sigma_star*sigma_star/2 + f_sample*sigma_star),1);

        sm_sample = halo_mass * (cosmo_params_stoc->OMb / cosmo_params_stoc->OMm) * f_sample; //f_star is galactic GAS/star fraction, so OMb is needed
    }
    else{
        sm_sample = halo_mass * (cosmo_params_stoc->OMb / cosmo_params_stoc->OMm) * fmin(fstar_mean,1);
    }

    sfr_mean = sm_sample / (astro_params_stoc->t_STAR * t_hubble(redshift));
    if(sigma_sfr > 0){
        sfr_sample = gsl_ran_ugaussian(rng);
        
        //Since there's no clipping on t_STAR (I think...), we can apply the lognormal to SFR directly instead of t_STAR
        sfr_sample = sfr_mean * exp(-sigma_sfr*sigma_sfr/2 + sfr_sample*sigma_sfr);
    }
    else{
        sfr_sample = sfr_mean;
    }

    //LOG_ULTRA_DEBUG("HM %.3e | SM %.3e | SFR %.3e (%.3e) | F* %.3e (%.3e) | duty %.3e",halo_mass,sm_sample,sfr_sample,sfr_mean,f_sample,fstar_mean,dutycycle_term);

    output[0] = sm_sample;
    output[1] = sfr_sample;

    return 0;
}

//props_in has form: M*, SFR, ....
int update_halo_properties(gsl_rng * rng, float redshift, float redshift_prev, float halo_mass, float halo_mass_prev, float *props_in, float *output){
    double f10 = astro_params_stoc->F_STAR10;
    double fa = astro_params_stoc->ALPHA_STAR;
    double sigma_star = astro_params_stoc->SIGMA_STAR;
    double sigma_sfr = astro_params_stoc->SIGMA_SFR;
    
    //sample new properties (uncorrelated)
    add_halo_properties(rng, halo_mass, redshift, output);

    //TODO; add correlation coeff + dependencies
    float corr = 0;
    float x1,x2,mu1,mu2;

    //STELLAR MASS: get median from mean + lognormal scatter (we leave off a bunch of constants and use the mean because we only need the ratio)
    mu1 = fmin(f10 * pow(halo_mass_prev/1e10,fa),1) * halo_mass_prev;
    mu2 = fmin(f10 * pow(halo_mass/1e10,fa),1) * halo_mass;
    //The same CDF value will be given by the ratio of the means/medians, since the scatter is z and M-independent
    x1 = props_in[0];
    x2 = mu2/mu1*x1;
    //interpolate between uncorrelated and matched properties.
    output[0] = output[0]*(1 - corr*x2/output[0]);

    //repeat for all other properties
    //SFR: get median (TODO: if I add z-M dependent scatters I will need to re-add the constants)
    mu1 = props_in[0] / t_hubble(redshift_prev);
    mu2 = output[0] / t_hubble(redshift);
    //calculate CDF(prop_prev|conditions) at previous snapshot (lognormal)
    x1 = props_in[1];
    x2 = mu2/mu1*x1;
    //interpolate between uncorrelated and matched properties.
    output[1] = output[1]*(1 - corr*x2/output[1]);

    //repeat for all other properties

    return 0;
}

//This is the function called to assign halo properties to an entire catalogue, used for DexM halos
int add_properties_cat(struct UserParams *user_params, struct CosmoParams *cosmo_params, struct AstroParams *astro_params, struct FlagOptions *flag_options
                        , int seed, float redshift, struct PerturbHaloField *halos){
    Broadcast_struct_global_STOC(user_params,cosmo_params,astro_params,flag_options);
    //allocate space for the properties
    int nhalos = halos->n_halos;
    halos->stellar_masses = (float *) calloc(nhalos,sizeof(float));
    halos->halo_sfr = (float *) calloc(nhalos,sizeof(float));

    //set up the rng
    gsl_rng * rng_stoc[user_params->N_THREADS];
    seed_rng_threads(rng_stoc,seed); 

    LOG_DEBUG("adding stars to %d halos",nhalos);

    //loop through the halos and assign properties
    int i;
    //TODO: update buffer when adding halo properties, make a #define or option with the number
    float buf[2];
#pragma omp parallel for private(buf)
    for(i=0;i<nhalos;i++){
        add_halo_properties(rng_stoc[omp_get_thread_num()], halos->halo_masses[i], redshift, buf);
        halos->stellar_masses[i] = buf[0];
        halos->halo_sfr[i] = buf[1];
        if(i<30) LOG_ULTRA_DEBUG("Halo %d, sm = %.3e, sfr = %.3e",i,buf[0],buf[1]);
    }

    free_rng_threads(rng_stoc);
    return 0;
}

//single sample of the HMF from inverse CDF method WIP
int sample_dndM_inverse(double x_in, gsl_rng * rng, double *result){
    //TODO: move this up the chain
    if(!user_params_stoc->USE_INTERPOLATION_TABLES){
        LOG_ERROR("Dont use inverse sampler without interpolation tables");
        Throw(ValueError);
    }
    
    double y_in;
    do{
       y_in = log(gsl_rng_uniform(rng)); 
    } while(y_in < MIN_LOGPROB);

    double z;

    z = gsl_spline2d_eval(inv_NgtrM_spline,x_in,y_in,inv_NgtrM_arg_acc,inv_NgtrM_prob_acc);
    if(!isfinite(z)){
        LOG_ERROR("interpolation failed! x=%.3e, y=%.3e z=%.3e",x_in,y_in,z);
        Throw(ValueError);
    }

    *result = z;
    
    return 0;
}

//single sample of the halo mass function using rejection method
int sample_dndM_rejection(double growthf, double delta, double Mmin, double Mmax, double M_filter, double ymin, double ymax, double sigma, gsl_rng * rng, double *result){
    double x1,y1, MassFunction;
    int c=0;

    while(c < MAX_ITERATIONS){
        //uniform sampling of logM between given limits
        x1 = Mmin + (Mmax-Mmin)*gsl_rng_uniform(rng);
        y1 = ymin + (ymax-ymin)*gsl_rng_uniform(rng);

        //for halo abundances (dNdlogM) from dfcoll/dM, M^1 for dlogm and M^-1 for dfcoll/dN
        MassFunction = dNdM_conditional(growthf,x1,M_filter,Deltac,delta,sigma);

        //LOG_ULTRA_DEBUG("%d || D %.1e | M1 %.1e | M2 %.1e | d %.1e | s %.1e -> %.1e ~~ %.1e from box((%.1e,%.1e,%.1e,%.1e)",
        //                c,growthf,exp(x1),exp(M_filter),delta,sigma,MassFunction,y1,exp(Mmin),exp(Mmax),ymin,ymax);
        if(y1<MassFunction){
            *result = x1;
            return 0;
        }
        
        c++;
    }
    LOG_ERROR("passed max iterations for rejection sampling, box(x1,x2,y1,y2) (%.3e,%.3e,%.3e,%.3e)",exp(Mmin),exp(Mmax),ymin,ymax);
    Throw(ValueError);
    return 0;
}

/* Creates a realisation of halo properties by sampling the halo mass function and 
 * conditional property PDFs, the number of halos is poisson sampled from the integrated CMF*/
int stoc_halo_sample(double growthf, double delta_lin, double delta_vol, double volume, double M_min, double M_max, int update, gsl_rng * rng, int *n_halo_out, float *hm_out){
    //delta check, if delta > deltacrit, make one big halo, if <-1 make no halos
    //both of these are possible with Lagrangian linear evolution
    if(delta_lin > Deltac){
        *n_halo_out = 1;
        hm_out[0] = M_max;
        return 0;
    }
    if(delta_lin < -1){
        *n_halo_out = 0;
        return 0;
    }
    
    double nh_buf,mu_lognorm;
    double hm_sample, sm_sample, sm_mean;
    double lnM_lo = log(M_min);
    double lnM_hi = log(M_max);
    double sigma_max = EvaluateSigma(lnM_hi);
    
    //these are the same at the end, n_halo incremented by poisson, halo_count incremented by updating halo list
    int n_halo = 0, halo_count = 0, nh;
    int ii;

    //TODO: set up mass distribution for sampling here
    //BEWARE: assuming max(dNdM) == dNdM(Mmin)
    double ymin,ymax;
    ymin = 0;
    ymax = dNdM_conditional(growthf,lnM_lo,lnM_hi,Deltac,delta_lin,sigma_max);

    double n_factor = volume * (RHOcrit * (1+delta_vol) / sqrt(2.*PI) * cosmo_params_stoc->OMm);

    //halo is too close to minimum to have progenitors or delta is too low to have halos
    if(ymax==0){
        *n_halo_out = 0;
        return 0;
    }

    //get average number of halos in cell n_order=0
    if(user_params_stoc->USE_INTERPOLATION_TABLES){
        if(update)
            nh_buf = EvaluatedNdMSpline(lnM_hi); //MMax set to cell size, delta given by redshift
        else
            nh_buf = EvaluatedNdMSpline(delta_lin); //delta set to Deltac, Mmax given by grid size

        //LOG_ULTRA_DEBUG("nh spline %.3e, nh integral %.3e V= %.2e M = %.2e",nh_buf*n_factor,IntegratedNdM(growthf, lnM_lo, lnM_hi, lnM_hi, delta_lin, 0, -1)*n_factor,volume,M_max);
    }
    else{
        nh_buf = IntegratedNdM(growthf, lnM_lo, lnM_hi, lnM_hi, delta_lin, 0, -1);
    }
    
    //constants to go from integral(1/M dFcol/dlogM) dlogM to integral(dNdlogM) dlogM
    nh_buf = nh_buf * n_factor;

    //sample poisson for stochastic halo number
    nh = gsl_ran_poisson(rng,nh_buf);
    n_halo += nh;

    LOG_ULTRA_DEBUG("Sampling %d || D %.2f d %.2f Mmin %.2e Mmax %.2e ymin %.2e ymax %.2e smax %.2f",nh,growthf,delta_lin,exp(lnM_lo),exp(lnM_hi),ymin,ymax,sigma_max);
    
    for(ii=0;ii<nh;ii++){
        //sample from the cHMF
        if(!user_params_stoc->STOC_INVERSE){
            sample_dndM_rejection(growthf,delta_lin,lnM_lo,lnM_hi,lnM_hi,ymin,ymax,sigma_max,rng,&hm_sample);
        }
        else{
            if(update){
                sample_dndM_inverse(lnM_hi,rng,&hm_sample);
            }
            else{
                sample_dndM_inverse(delta_lin,rng,&hm_sample);
            }
        }

        hm_sample = exp(hm_sample);

        hm_out[halo_count] = hm_sample;
        halo_count++;
    }

    *n_halo_out = n_halo;

    LOG_ULTRA_DEBUG("sampled (%d) halos from %.2f | Mfilt = %.3e | delta_l = %.2f | delta_v = %.2f | V = %.2f",n_halo,nh_buf,M_max,delta_lin,delta_vol,volume);
    LOG_ULTRA_DEBUG("first few masses %.2e %.2e %.2e",hm_out[0],hm_out[1],hm_out[2]);
    return 0;
}

/* Creates a realisation of halo properties by sampling the halo mass function and 
 * conditional property PDFs, Sampling is done until there is no more mass in the condition
 * Stochasticity is ignored below a certain mass threshold*/
int stoc_mass_sample(double z_out, double growth_out, double param, double M_max, double M_min, double ps_ratio, int update, gsl_rng * rng, float *M_out, int *n_halo_out){
    int n_halo=0;
    double delta_lin;

    //dNdM sampling currently does (delta_in - Deltac)/growth_1
    //for update: param = growth_in, for cells param = delta_lin
    if(update){
        delta_lin = Deltac * growth_out / param;
    }
    else{
        delta_lin = param;
    }

    //delta check, if delta > deltacrit, make one big halo, if <-1 make no halos
    //both of these are possible with Lagrangian linear evolution
    if(delta_lin >= Deltac){
        *n_halo_out = 1;
        M_out[0] = M_max;
        return 0;
    }
    if(delta_lin < -1 || M_max <= M_min){
        *n_halo_out = 0;
        return 0;
    }
    
    double lnMmin = log(M_min);
    double lnMmax = log(M_max);
    double sigma_max = EvaluateSigma(lnMmax);

    double frac = EvaluateFgtrM(z_out,lnMmin,delta_lin/growth_out,sigma_max);

    //Set the minimum mass we care about for this condition
    /*while(frac < 0.01){
        lnMmin = lnMmin + 0.5; //* exp(0.5)
        frac = EvaluateFgtrM(z_out,lnMmin,delta_lin/growth_out,sigma_max);
    }
    Mmin = exp(lnMmin);*/

    //we apply the ps ratio here, since it won't effect the CMF, and should just scale everything (capped at Fcoll > 1)
    //we still allow the final sample to go beyond M_remaining, but not beyond M_max which stops Fcoll > 1
    //TODO: This would be more efficient if I used FgtrM straight from the desired HMF
    double M_remaining = M_max * frac / ps_ratio;
    M_remaining = M_remaining > M_max ? M_max : M_remaining;

    //BEWARE: assuming max(dNdM) == dNdM(Mmin) for the rejection sampler
    //This is not true for very high deltas, where there is a spike near the condition mass
    double ymin,ymax;
    if(!user_params_stoc->STOC_INVERSE){
        ymin = 0;
        ymax = dNdM_conditional(growth_out,lnMmin,lnMmax,Deltac,delta_lin,sigma_max);
    }

    LOG_ULTRA_DEBUG("Condition M = %.2e (%.2e), sigma = %.2e, Mmin = %.2e, delta = %.2f",M_max,M_remaining,sigma_max,M_min,M_min,delta_lin);

    int attempts = 0;
    double M_prog = 0.;
    double M_sample;
    while(M_remaining > M_min){
        if(!user_params_stoc->STOC_INVERSE){
            sample_dndM_rejection(growth_out,delta_lin,lnMmin,lnMmax,lnMmax,ymin,ymax,sigma_max,rng,&M_sample);
        }
        else{
            if(update){
                sample_dndM_inverse(lnMmax,rng,&M_sample);
            }
            else{
                sample_dndM_inverse(delta_lin,rng,&M_sample);
            }
        }
        M_sample = exp(M_sample);

        //sometimes we sample more than the remaining mass from dndM, if we re-draw or cap at remaining mass this gives too many small halos
        //and if we just keep it there's a possibility for spontaneous mass creation. This is the maximum cap without having to change previous samples
        //above Mmin, essentially we throw out progenitors below our minimum to make up the mass
        M_sample = M_sample > (M_max - M_prog) ? (M_max - M_prog) : M_sample;
        
        //LOG_ULTRA_DEBUG("sampled a progenitor %.3e, %.3e",M_sample,M_remaining);
        attempts++;

        //attempts is total progenitors here, including those under the mass limit
        //if we've sampled more than the max number of possible halos, something went wrong
        if(attempts >= M_max / M_min){
            LOG_ERROR("sampler hit max number %.1f of progenitors, Mmin = %.3e, Mmax = %.3e",M_max/M_min,M_min,M_max);
            Throw(ValueError);
        }

        M_remaining -= M_sample;
        if(M_sample > M_min)
        {
            M_out[n_halo++] = M_sample;
            M_prog += M_sample;
        }
    }
    *n_halo_out = n_halo;
    return 0;
}

// will have to add properties here and output grids, instead of in perturbed
int build_halo_cats(gsl_rng **rng_stoc, double redshift, bool eulerian, float *dens_field, int *n_halo_out, int *halo_coords, float *halo_masses, float *stellar_masses, float *halo_sfr){    
    double growthf = dicke(redshift);
    int lo_dim = user_params_stoc->HII_DIM;
    int hi_dim = user_params_stoc->DIM;
    double boxlen = user_params_stoc->BOX_LEN;
    //cell size for smoothing / CMF calculation
    double cell_size_R = boxlen / lo_dim * L_FACTOR;
    //cell size for volume calculation
    double cell_size_L = boxlen / lo_dim;

    double volume = cell_size_L * cell_size_L * cell_size_L;
    double ps_ratio = 1.;

    double Mmax_meandens = RtoM(cell_size_R);
    double Mmin, Mmin_s;

    int x,y,z,i;

    Mmin = minimum_source_mass(redshift,astro_params_stoc,flag_options_stoc);

    init_ps();
    if(user_params_stoc->USE_INTERPOLATION_TABLES){
        initialiseSigmaMInterpTable(Mmin,MMAX_TABLES);
        initialise_dNdM_table(-1, Deltac, growthf, log(Mmax_meandens), log(Mmin), false); //WONT WORK WITH EULERIAN
        if(user_params_stoc->STOC_INVERSE){
            initialise_inverse_table(-1, Deltac, log(Mmin), growthf, 0., false);
        }
    }

    //Since the conditional MF is extended press-schecter, we rescale by a factor equal to the ratio of the collapsed fractions (n_order == 1) of the UMF
    if(user_params_stoc->HMF!=0){
        ps_ratio = (IntegratedNdM(growthf,log(Mmin),log(Mmax_meandens),log(Mmax_meandens),0,1,0) 
            / IntegratedNdM(growthf,log(Mmin),log(Mmax_meandens),log(Mmax_meandens),0,1,user_params_stoc->HMF));
        volume = volume / ps_ratio;
    }

    LOG_DEBUG("Beginning stochastic halo sampling on %d ^3 grid",lo_dim);
    LOG_DEBUG("z = %f, Mmin = %e, Mmax = %e,volume = %.3e (%.3e), cell length = %.3e R = %.3e D = %.3e",redshift,Mmin,Mmax_meandens,volume,Mmax_meandens/RHOcrit/cosmo_params_stoc->OMm,cell_size_L,cell_size_R,dicke(redshift));
    
    //shared halo count
    int counter = 0;
    float *hm_buf;

#pragma omp parallel private(x,y,z,i,hm_buf) num_threads(user_params_stoc->N_THREADS)
    {
        //PRIVATE VARIABLES
        int threadnum = omp_get_thread_num();
        //buffers per cell
        hm_buf = (float *)calloc(MAX_HALO_CELL,sizeof(float));
        int nh_buf=0;
        double cell_hm;        
        double delta_v;
        double delta_l;
        double Mmax;
        double randbuf;
        float prop_buf[2];

        //debug printing
        int print_counter = 0;

        //highres conversion
        int x_crd,y_crd,z_crd;

#pragma omp for
        for (x=0; x<lo_dim; x++){
            for (y=0; y<lo_dim; y++){
                for (z=0; z<lo_dim; z++){
                    //cell_hm = 0;
                    delta_v = (double)dens_field[HII_R_INDEX(x,y,z)];

                    //If Eulerian densities given, calculate linear from Mo & White 96 formula, total mass given by volume & nonlinear dens
                    if(eulerian){
                        Mmax = Mmax_meandens * (1+delta_v);                
                        delta_l = -1.35*pow(1+delta_v,-2./3.) + 0.78785*pow(1+delta_v,-0.58661) - 1.12431*pow(1+delta_v,-0.5) + 1.68647;
                    }
                    //If Lagrangian (IC) densities given, calculate linear with D(z), 
                    else{
                        Mmax = Mmax_meandens;
                        delta_l = delta_v * growthf;
                        delta_v = 0.;
                    }

                    LOG_ULTRA_DEBUG("Starting sample %d (%d) with delta (l,v) = (%.2f %.2f) cell (V=%.2e), from %.2e to %.2e"
                                    ,x*lo_dim*lo_dim + y*lo_dim + z,lo_dim*lo_dim*lo_dim,delta_l,delta_v,volume,Mmin,Mmax);
                    if(user_params_stoc->STOC_MASS_SAMPLING)
                        stoc_mass_sample(redshift, growthf, delta_l, Mmax, Mmin, ps_ratio, 0, rng_stoc[threadnum], hm_buf, &nh_buf);
                    else
                        stoc_halo_sample(growthf,delta_l,delta_v,volume,Mmin,Mmax,0,rng_stoc[threadnum],&nh_buf,hm_buf);

                    //output total halo number, catalogues of masses and positions
                    for(i=0;i<nh_buf;i++){
                        if(hm_buf[i]==0){
                            LOG_ERROR("zeromass halo.");
                            Throw(ValueError);
                        }

                        add_halo_properties(rng_stoc[threadnum], hm_buf[i], redshift, prop_buf);

                        //we want to randomly place each halo within each lores cell,then map onto hires
                        //this is so halos are on DIM grids to match HaloField and Perturb options
                        randbuf = gsl_rng_uniform(rng_stoc[threadnum]);
                        x_crd = (int)((x + randbuf) / (float)(lo_dim) * (float)(hi_dim));
                        randbuf = gsl_rng_uniform(rng_stoc[threadnum]);
                        y_crd = (int)((y + randbuf) / (float)(lo_dim) * (float)(hi_dim));
                        randbuf = gsl_rng_uniform(rng_stoc[threadnum]);
                        z_crd = (int)((z + randbuf) / (float)(lo_dim) * (float)(hi_dim));

                        //cell_hm += hm_buf[i];

                        //fill in arrays now, this should be quick compared to the sampling so critical shouldn't slow this down much
                        #pragma omp critical
                        {
                            halo_masses[counter] = hm_buf[i];
                            stellar_masses[counter] = prop_buf[0];
                            halo_sfr[counter] = prop_buf[1];
                            halo_coords[0 + 3*counter] = x_crd;
                            halo_coords[1 + 3*counter] = y_crd;
                            halo_coords[2 + 3*counter] = z_crd;
                            counter++;
                        }
                    }
                    LOG_ULTRA_DEBUG("First few Masses of %d %.3e %.3e %.3e",nh_buf,hm_buf[0],hm_buf[1],hm_buf[2]);
                }
            }
        }

    //need barrier before free
    #pragma omp barrier
    free(hm_buf);
    }
    *n_halo_out = counter;
    //LOG_DEBUG("first few halo masses of %d %.3e %.3e %.3e",counter,halo_masses[0],halo_masses[1],halo_masses[2]);
    //LOG_DEBUG("first few stellar masses %.3e %.3e %.3e",stellar_masses[0],stellar_masses[1],stellar_masses[2]);
    if(user_params_stoc->USE_INTERPOLATION_TABLES){
        freeSigmaMInterpTable();
        free_dNdM_table();
        if(user_params_stoc->STOC_INVERSE){
            free_inverse_table();
        }
    }

    return 0;
}
//updates halo masses (going increasing redshift, z_prev > z)
int halo_update(gsl_rng ** rng, double z_in, double z_out, int nhalo_in, int *halocoords_in, float *halomass_in, float *stellarmass_in, float *sfr_in,
                 int *nhalo_out, int *halocoords_out, float *halomass_out, float *stellarmass_out, float *sfr_out){

    if(z_in >= z_out){
        LOG_ERROR("halo update must go backwards in time!!! z_in = %.1f, z_out = %.1f",z_in,z_out);
        Throw(ValueError);
    }
    if(nhalo_in == 0){
        LOG_DEBUG("No halos to update, continuing...");
        *nhalo_out = 0;
        return 0;
    }
    double growth_in = dicke(z_in);
    double growth_out = dicke(z_out);

    double Mmin = minimum_source_mass(z_out,astro_params_stoc,flag_options_stoc);
    //If we are inverse number sampling, these need to be the same,
    //if we are inverse mass sampling, we apply the factor
    //for rejection sampling it doesn't matter

    double delta_lin = Deltac * growth_out / growth_in;
    double delta_vol = 0;

    //LOG_DEBUG("first few masses: %.2e %.2e %.2e", halomass_in[0], halomass_in[1], halomass_in[2]);

    init_ps();
    if(user_params_stoc->USE_INTERPOLATION_TABLES){
        initialiseSigmaMInterpTable(Mmin,MMAX_TABLES);
        initialise_dNdM_table(log(Mmin), log(MMAX_TABLES), growth_out, growth_in, log(Mmin), true);
        if(user_params_stoc->STOC_INVERSE){
            initialise_inverse_table(log(Mmin), log(MMAX_TABLES), log(Mmin), growth_out, growth_in, true);
        }
    }

    LOG_DEBUG("Updating halo cat: z_in = %f, z_out = %f (d = %f), n_in = %d  Mmin = %e",z_in,z_out,delta_lin,nhalo_in,Mmin);

    int count = 0;
    float *halo_buf;

#pragma omp parallel private(halo_buf) num_threads(user_params_stoc->N_THREADS)
    {
        //allocate halo buffer, one halo splitting into >1000 in one step would be crazy I think
        halo_buf = (float *)calloc(MAX_HALO_CELL,sizeof(double));
        int n_prog;
        
        float propbuf_in[2];
        float propbuf_out[2];
        int threadnum = omp_get_thread_num();
        float M2,sm2,sfr2;
        int ii,jj,x,y,z;
        double volume,R;
        double M_sum;
        double ps_ratio = 1.;

#pragma omp for
        for(ii=0;ii<nhalo_in;ii++){
            //get halo information
            M_sum = 0.;
            M2 = halomass_in[ii];
            sm2 = stellarmass_in[ii];
            sfr2 = sfr_in[ii];
            x = halocoords_in[3*ii + 0];
            y = halocoords_in[3*ii + 1];
            z = halocoords_in[3*ii + 2];

            volume = M2 / RHOcrit / cosmo_params_stoc->OMm;
            
            if(user_params_stoc->HMF!=0){
                ps_ratio = (IntegratedNdM(growth_out,log(Mmin),log(M2),log(M2),0,1,0) 
                   / IntegratedNdM(growth_out,log(Mmin),log(M2),log(M2),0,1,user_params_stoc->HMF));
                volume = volume / ps_ratio;
            }

            //find progenitor halos
            LOG_ULTRA_DEBUG("halo %d of %d, M=%.2e M*=%.2e, sfr = %.2e",ii,nhalo_in,M2,sm2,sfr2);
            if(user_params_stoc->STOC_MASS_SAMPLING)
                stoc_mass_sample(z_out,growth_out,growth_in,M2,Mmin,ps_ratio,1,rng[threadnum],halo_buf,&n_prog);
            else
                stoc_halo_sample(growth_out, delta_lin, delta_vol, volume, Mmin, M2, 1, rng[threadnum], &n_prog, halo_buf);
            
            LOG_ULTRA_DEBUG("Found %d progenitors first few masses %.2e %.2e %.2e",n_prog,halo_buf[0],halo_buf[1],halo_buf[2]);

            //place progenitors in local list
            for(jj=0;jj<n_prog;jj++){
                propbuf_in[0] = sm2;
                propbuf_in[1] = sfr2;
                update_halo_properties(rng[threadnum], z_out, z_in, halo_buf[jj], M2, propbuf_in, propbuf_out);

                M_sum += halo_buf[jj];
                //TODO: proper checks
                if(M_sum > (M2)*(1+1e-5)){
                    LOG_SUPER_DEBUG("halo_in %d (%d), progenitor mass %.2e (%.2e) > descendant mass %.2e",ii,jj,M_sum,halo_buf[jj],M2);
                }
                
                #pragma omp critical
                {
                    halomass_out[count] = halo_buf[jj];

                    //the halos should already be on the hires grid
                    halocoords_out[3*count + 0] = x;
                    halocoords_out[3*count + 1] = y;
                    halocoords_out[3*count + 2] = z;
                    
                    stellarmass_out[count] = propbuf_out[0];
                    sfr_out[count] = propbuf_out[1];
                    count++;
                }
            }
        }
        //don't want to free before all threads are done
        #pragma omp barrier
        free(halo_buf);
    }

    if(user_params_stoc->USE_INTERPOLATION_TABLES){
        freeSigmaMInterpTable();
        free_dNdM_table();
        if(user_params_stoc->STOC_INVERSE){
            free_inverse_table();
        }
    }
    *nhalo_out = count;
    return 0;
}


//function that talks between the structures (Python objects) and the sampling functions
int stochastic_halofield(struct UserParams *user_params, struct CosmoParams *cosmo_params, struct AstroParams *astro_params, struct FlagOptions *flag_options
                        , int seed, float redshift_prev, float redshift, bool eulerian, float *dens_field, struct HaloField *halos_prev, struct HaloField *halos){
    Broadcast_struct_global_UF(user_params,cosmo_params);
    Broadcast_struct_global_PS(user_params,cosmo_params);
    Broadcast_struct_global_STOC(user_params,cosmo_params,astro_params,flag_options);

    int n_halo_stoc;
    int i_start,i;

    int * halo_coords = (int*)calloc(MAX_HALO,sizeof(int)*3);
    float * halo_masses = (float*)calloc(MAX_HALO,sizeof(float));
    float * stellar_masses = (float*)calloc(MAX_HALO,sizeof(float));
    float * halo_sfr = (float*)calloc(MAX_HALO,sizeof(float));

    //set up the rng
    gsl_rng * rng_stoc[user_params->N_THREADS];
    seed_rng_threads(rng_stoc,seed);

    //fill the hmf to possibly avoid deallocation issues:
    //TODO: actually fill the HMF in the sampling functions
    init_hmf(halos);
    if(halos->first_box)
        init_hmf(halos_prev);

    //Fill them
    if(halos->first_box){
        LOG_DEBUG("building first halo field at z=%.1f", redshift);
        build_halo_cats(rng_stoc,(double)redshift,eulerian,dens_field,&n_halo_stoc,halo_coords,halo_masses,stellar_masses,halo_sfr);
    }
    else{
        LOG_DEBUG("updating halo field from z=%.1f to z=%.1f | %d", redshift_prev,redshift,halos->n_halos);
        halo_update(rng_stoc,redshift_prev,redshift,halos_prev->n_halos,halos_prev->halo_coords,halos_prev->halo_masses,
                    halos_prev->stellar_masses,halos_prev->halo_sfr,&n_halo_stoc,halo_coords,halo_masses,stellar_masses,halo_sfr);
    }

    //trim buffers to the correct number of halos
    if(n_halo_stoc > 0){
        halo_coords = (int*) realloc(halo_coords,sizeof(int)*3*n_halo_stoc);
        halo_masses = (float*) realloc(halo_masses,sizeof(float)*n_halo_stoc);
        stellar_masses = (float*) realloc(stellar_masses,sizeof(float)*n_halo_stoc);
        halo_sfr = (float*) realloc(halo_sfr,sizeof(float)*n_halo_stoc);
    }
    else{
        //one dummy entry for saving
        halo_coords = (int*) realloc(halo_coords,sizeof(int)*3);
        halo_masses = (float*) realloc(halo_masses,sizeof(float));
        stellar_masses = (float*) realloc(stellar_masses,sizeof(float));
        halo_sfr = (float*) realloc(halo_sfr,sizeof(float));
    }

    //assign the pointers to the structs
    halos->n_halos = n_halo_stoc;
    halos->halo_masses = halo_masses;
    halos->halo_coords = halo_coords;
    halos->stellar_masses = stellar_masses;
    halos->halo_sfr = halo_sfr;

    LOG_DEBUG("Found %d Halos", halos->n_halos);
    // if (halos->n_halos > 3){
    //     LOG_DEBUG("pointer Halo Masses: %.3e %.3e %.3e %.3e", halo_masses[0], halo_masses[1], halo_masses[2], halo_masses[3]);
    //     LOG_DEBUG("struct Halo Masses: %.3e %.3e %.3e %.3e", halos->halo_masses[0], halos->halo_masses[1], halos->halo_masses[2], halos->halo_masses[3]);
    //     LOG_DEBUG("pointer Stellar Masses: %.3e %.3e %.3e %.3e", stellar_masses[0], stellar_masses[1], stellar_masses[2], stellar_masses[3]);
    //     LOG_DEBUG("struct Stellar Masses: %.3e %.3e %.3e %.3e", halos->stellar_masses[0], halos->stellar_masses[1], halos->stellar_masses[2], halos->stellar_masses[3]);
    //     LOG_DEBUG("pointer SFR: %.3e %.3e %.3e %.3e", halo_sfr[0], halo_sfr[1], halo_sfr[2], halo_sfr[3]);
    //     LOG_DEBUG("struct SFR: %.3e %.3e %.3e %.3e", halos->halo_sfr[0], halos->halo_sfr[1], halos->halo_sfr[2], halos->halo_sfr[3]);
    // }

    free_rng_threads(rng_stoc);
    return 0;
}

//This, for the moment, grids the PERTURBED halo catalogue.
//TODO: make a way to output both types by making 2 wrappers to this function that pass in arrays rather than structs
int ComputeHaloBox(double redshift, struct UserParams *user_params, struct CosmoParams *cosmo_params, struct AstroParams *astro_params
                    , struct FlagOptions * flag_options, struct PerturbedField * perturbed_field, struct PerturbHaloField *halos
                    , struct HaloBox *grids){
    int status;
    Try{
        LOG_DEBUG("Gridding %d halos...",halos->n_halos);

        //get parameters
        Broadcast_struct_global_UF(user_params,cosmo_params);
        Broadcast_struct_global_PS(user_params,cosmo_params);
        double alpha_esc = astro_params->ALPHA_ESC;
        double norm_esc = astro_params->F_ESC10;
        double hm_avg=0,sm_avg=0,sfr_avg=0;
        double hm_expected,sm_expected,sfr_expected;

        double growth_z;
        double M_min,M_max,lnMmin,lnMmax;
        double alpha_star,norm_star,t_star,Mlim_Fstar,Mlim_Fesc;
        double volume,sigma_max;
        double prefactor_mass,prefactor_sfr,prefactor_star;

        //TODO: interpolation tables (eh this is fast anyway)
        //TODO: PS_RATIO Term

        //calculate expected average halo box, for mean halo box fixing and debugging
        if(!flag_options->HALO_STOCHASTICITY || LOG_LEVEL >= DEBUG_LEVEL){
            init_ps();

            growth_z = dicke(redshift);
            alpha_star = astro_params->ALPHA_STAR;
            norm_star = astro_params->F_STAR10;
            t_star = astro_params->t_STAR;

            volume = pow(user_params->BOX_LEN/user_params->HII_DIM,3);
            
            M_max = volume * RHOcrit * cosmo_params->OMm;
            lnMmax = log(M_max);
            M_min = minimum_source_mass(redshift,astro_params,flag_options);
            lnMmin = log(M_min);
            sigma_max = EvaluateSigma(lnMmax);

            prefactor_mass = volume * RHOcrit * cosmo_params->OMm / sqrt(2.*PI);
            prefactor_star = volume * RHOcrit * cosmo_params->OMb * norm_star * norm_esc;
            prefactor_sfr = volume * RHOcrit * cosmo_params->OMb * norm_star / t_star / t_hubble(redshift);
            
            Mlim_Fstar = Mass_limit_bisection(M_min, global_params.M_MAX_INTEGRAL, alpha_star, norm_star);
            Mlim_Fesc = Mass_limit_bisection(M_min, global_params.M_MAX_INTEGRAL, alpha_esc, norm_esc);

            if(user_params->USE_INTERPOLATION_TABLES){
                initialiseSigmaMInterpTable(M_min,MMAX_TABLES);
                //More Stuff Here SFRD AND NION TABLES FROM TS.C BUT BE CAREFUL
            }

            hm_expected = IntegratedNdM(growth_z, lnMmin, log(global_params.M_MAX_INTEGRAL),log(global_params.M_MAX_INTEGRAL),0,1,user_params->HMF);
            sm_expected = Nion_General(redshift, M_min, astro_params->M_TURN, alpha_star, alpha_esc, norm_star, norm_esc, Mlim_Fstar, Mlim_Fesc);
            sfr_expected = Nion_General(redshift, M_min, astro_params->M_TURN, alpha_star, 0., norm_star, 1., Mlim_Fstar, 0.);

            hm_expected *= volume;
            sm_expected *= prefactor_star;
            sfr_expected *= prefactor_sfr;
        }

        //do the mean HMF box    
        if(!flag_options->HALO_STOCHASTICITY){
            LOG_DEBUG("Mean halo boxes || Mmin = %.2e | Mmax = %.2e (s=%.2e) | z = %.2e | D = %.2e | vol = %.2e",M_min,M_max,sigma_max,redshift,growth_z,volume);
#pragma omp parallel num_threads(user_params->N_THREADS)
            {
                int i;
                double dens, dens_l;
                double mass, wstar, sfr;
                double lnMmax_cell, sigmax_cell, M_max_cell;
#pragma omp for reduction(+:hm_avg,sm_avg,sfr_avg)
                for(i=0;i<HII_TOT_NUM_PIXELS;i++){
                    dens = perturbed_field->density[i];

                    //quick hack: I'm using STOC_INVERSE to toggle between default 21cmfast halos and expected integral
                    if(user_params->STOC_INVERSE){
                        //The default 21cmFAST has a strange behaviour where the nonlinear density is used as linear,
                        //the condition mass is at mean density, but the total cell mass is multiplied by delta 
                        dens_l = dens;
                        M_max_cell = M_max;
                    }
                    else{
                        //Mo & White 1996 nonlinear to linear conversion
                        //NOTE: This seems to not work very well, overpredicting by a lot at high redshift 
                        //I may remove this in the future
                        dens_l = (-1.35*pow(1+dens,-2./3.) + 0.78785*pow(1+dens,-0.58661)
                            - 1.12431*pow(1+dens,-0.5) + 1.68647);
                        //M_max_cell = M_max * (1+dens); //sets condition mass
                        M_max_cell = M_max;
                        //dens = 0;
                    }

                    //ignore very low density
                    if(M_max_cell < M_min || dens_l <= -1 || dens <= -1 || dens_l != dens_l){
                        mass = 0.;
                        wstar = 0.;
                        sfr = 0.;
                    }
                    //turn into one large halo if we exceed the critical
                    else if(dens_l>=Deltac){                        
                        mass = M_max * (1+dens);
                        wstar = M_max * (1+dens) * cosmo_params->OMb / cosmo_params->OMm * norm_star * pow(M_max*(1+dens)/1e10,alpha_star) * norm_esc * pow(M_max*(1+dens)/1e10,alpha_esc);
                        sfr = M_max * (1+dens) * cosmo_params->OMb / cosmo_params->OMm * norm_star * pow(M_max*(1+dens)/1e10,alpha_star) / t_star / t_hubble(redshift);
                    }
                    else{                        
                        lnMmax_cell = log(M_max_cell);
                        sigmax_cell = EvaluateSigma(lnMmax_cell);

                        //calling IntegratedNdM with star and SFR need special care for the f*/fesc clipping, and calling NionConditionalM for mass includes duty cycle
                        //neither of which I want
                        mass = IntegratedNdM(growth_z,lnMmin,lnMmax_cell,lnMmax_cell,dens_l,1,-1) * prefactor_mass * (1+dens);

                        wstar = Nion_ConditionalM(growth_z, lnMmin, lnMmax_cell, sigmax_cell, Deltac, dens_l, astro_params->M_TURN
                                                , astro_params->ALPHA_STAR, astro_params->ALPHA_ESC, astro_params->F_STAR10, astro_params->F_ESC10
                                                , Mlim_Fstar, Mlim_Fesc, user_params->FAST_FCOLL_TABLES) * prefactor_star * (1+dens);

                        sfr = Nion_ConditionalM(growth_z, lnMmin, lnMmax_cell, sigmax_cell, Deltac, dens_l, astro_params->M_TURN
                                                , astro_params->ALPHA_STAR, 0., astro_params->F_STAR10, 1., Mlim_Fstar, 0.
                                                , user_params->FAST_FCOLL_TABLES) * prefactor_sfr * (1+dens);
                    }

                    grids->halo_mass[i] = mass;
                    grids->wstar_mass[i] = wstar;
                    grids->halo_sfr[i] = sfr;
                    
                    if(user_params->STOC_INVERSE || LOG_LEVEL >= DEBUG_LEVEL){
                        hm_avg += mass;
                        sm_avg += wstar;
                        sfr_avg += sfr;
                    }
                }
            }
            
            hm_avg /= HII_TOT_NUM_PIXELS;
            sm_avg /= HII_TOT_NUM_PIXELS;
            sfr_avg /= HII_TOT_NUM_PIXELS;

            if(user_params->STOC_INVERSE){
                //This is the adjustment that happens in the rest of the code
                int i;
                for(i=0;i<HII_TOT_NUM_PIXELS;i++){
                    grids->halo_mass[i] *= hm_expected/hm_avg;
                    grids->wstar_mass[i] *= sm_expected/sm_avg;
                    grids->halo_sfr[i] *= sfr_expected/sfr_avg;
                }
                
                hm_avg = hm_expected;
                sm_avg = sm_expected;
                sfr_avg = sfr_expected;
            }

            if(user_params->USE_INTERPOLATION_TABLES){
                freeSigmaMInterpTable();
            }
        }
        else{

#pragma omp parallel num_threads(user_params->N_THREADS)
            {
                int i_halo,idx,x,y,z;
                double m,wstar,sfr,fesc;
#pragma omp for
                for (idx=0; idx<HII_TOT_NUM_PIXELS; idx++) {
                    grids->halo_mass[idx] = 0.0;
                    grids->wstar_mass[idx] = 0.0;
                    grids->halo_sfr[idx] = 0.0;
                }

#pragma omp barrier

#pragma omp for reduction(+:hm_avg,sm_avg,sfr_avg)
                for(i_halo=0; i_halo<halos->n_halos; i_halo++){
                    x = halos->halo_coords[0+3*i_halo];
                    y = halos->halo_coords[1+3*i_halo];
                    z = halos->halo_coords[2+3*i_halo];

                    m = halos->halo_masses[i_halo];

                    //This clipping is normally done with the mass_limit_bisection root find. 
                    //TODO: It could save some `pow` calls if I compute a mass limit outside the loop
                    fesc = fmin(norm_esc*pow(m/1e10,alpha_esc),1);

                    wstar = halos->stellar_masses[i_halo]*fesc;
                    sfr = halos->halo_sfr[i_halo];
                    //will probably need weighted SFR, unweighted Stellar later on
                    //Lx as well when that scatter is included

    #pragma omp atomic update
                    grids->halo_mass[HII_R_INDEX(x, y, z)] += m;
    #pragma omp atomic update
                    grids->wstar_mass[HII_R_INDEX(x, y, z)] += wstar;
    #pragma omp atomic update
                    grids->halo_sfr[HII_R_INDEX(x, y, z)] += sfr;

                    if(LOG_LEVEL >= DEBUG_LEVEL){
                        hm_avg += m;
                        sm_avg += wstar;
                        sfr_avg += sfr;
                    }
                }
            }
            if(LOG_LEVEL > DEBUG_LEVEL){
                hm_avg /= HII_TOT_NUM_PIXELS;
                sm_avg /= HII_TOT_NUM_PIXELS;
                sfr_avg /= HII_TOT_NUM_PIXELS;
            }
        }
        LOG_DEBUG("HaloBox Cells:  %9.2e %9.2e %9.2e %9.2e", grids->halo_mass[0], grids->halo_mass[1]
            , grids->halo_mass[2] , grids->halo_mass[3]);
        LOG_DEBUG("Stellar Masses: %9.2e %9.2e %9.2e %9.2e", grids->wstar_mass[0], grids->wstar_mass[1]
            , grids->wstar_mass[2] , grids->wstar_mass[3]);
        LOG_DEBUG("Halo SFR:       %9.2e %9.2e %9.2e %9.2e", grids->halo_sfr[0], grids->halo_sfr[1]
            , grids->halo_sfr[2] , grids->halo_sfr[3]);
            
        LOG_DEBUG("Redshift %.2f: Expected averages: (%11.3e, %11.3e, %11.3e) || Box averages (%11.3e,%11.3e,%11.3e)"
                    ,redshift,hm_expected,sm_expected,sfr_expected,hm_avg,sm_avg,sfr_avg);
    }
    Catch(status){
        return(status);
    }
    LOG_DEBUG("Done.");
    return(0);
    
}

//testing function to print stuff out from python
/* type==0: UMF/CMF
 * type==1: Integrated CMF in a single condition at multiple masses N(>M)
 * type==2: Integrated CMF in multiple conditions in the entire mass range
 * type==3: Halo catalogue from single condition (testing stoc_halo/mass_sample)
 * type==4: Not implemented
 * type==5: halo catalogue given a list of conditions (testing build_halo_cats/halo_update)
 * type==6: output halo mass given probabilities from inverse CMF tables*/
int my_visible_function(struct UserParams *user_params, struct CosmoParams *cosmo_params, struct AstroParams *astro_params, struct FlagOptions *flag_options
                        , int seed, int n_mass, float *M, bool update, double condition, double z_out, double z_in, int type, double *result){
    int status;
    Try{
        //make the global structs
        Broadcast_struct_global_UF(user_params,cosmo_params);
        Broadcast_struct_global_PS(user_params,cosmo_params);
        Broadcast_struct_global_STOC(user_params,cosmo_params,astro_params,flag_options);

        omp_set_num_threads(user_params->N_THREADS);
        
        //set up the rng
        gsl_rng * rng_stoc[user_params->N_THREADS];
        seed_rng_threads(rng_stoc,seed);

        //gsl_rng * rseed = gsl_rng_alloc(gsl_rng_mt19937); // An RNG for generating seeds for multithreading

        //gsl_rng_set(rseed, random_seed);
        double test;
        int err=0;
        int i;

        double growth_out = dicke(z_out);
        double growth_in = dicke(z_in);
        double Mmin = minimum_source_mass(z_out,astro_params,flag_options);

        double Mmax, volume, R, delta_l, delta_v;
        bool eulerian = false; //TODO:proper option even though I don't use eulerian much
        double ps_ratio;

        //Here the condition is a mass, volume is the Lagrangian volume and delta_l is set by the
        //redshift difference which represents the difference in delta_crit across redshifts
        if(update){
            Mmax = condition;
            delta_l = Deltac * growth_out / growth_in;
            R = MtoR(Mmax);
            volume = 4. / 3. * PI * R * R * R;
            delta_v = 0;
        }
        //Here the condition is a cell of a given density, the volume/mass is given by the grid parameters
        //below build_halo_cats(), the delta at z_out is expected, above, the IC (z=0) delta is expected.
        //we pass in the IC delta (so we can compare populations across redshifts) and adjust here
        else{
            R = user_params->BOX_LEN / user_params->HII_DIM * L_FACTOR;
            volume = pow(user_params->BOX_LEN / user_params->HII_DIM,3);
            Mmax = RtoM(R);
            if(eulerian){
                //Mo & White 1996 TODO:put in function
                //convert Eulerian Pertubed overdensity to Lagrangian overdensity
                //the mass in the cell (and hence R) is scaled by the eulerian density, the CMF uses lagrangian
                delta_l = -1.35*pow(1+condition,-2./3.) + 0.78785*pow(1+condition,-0.58661)
                        - 1.12431*pow(1+condition,-0.5) + 1.68647;
                delta_v = condition;
                Mmax = Mmax*(1+delta_v);
            }
            else{
                delta_l = condition;
                delta_v = 0;
            }
        }

        double lnMmin = log(Mmin);
        double lnMmax = log(Mmax);

        LOG_DEBUG("TEST FUNCTION: type = %d up %d, z = (%.2f,%.2f), Mmin = %e, Mmax = %e, R = %.2e (%.2e), delta(l,v) = (%.2f,%.2f), M(%d)=[%.2e,%.2e,%.2e...]",type,update,z_out,z_in,Mmin,Mmax,R,volume,delta_l,delta_v,n_mass,M[0],M[1],M[2]);

        //don't do anything for delta outside range               
        if(delta_l > Deltac || delta_l < -1){
            LOG_ERROR("delta of %f is out of bounds",delta_l);
            Throw(ValueError);
        }

        if(type != 4 && type !=5){
            init_ps();
            if(user_params_stoc->USE_INTERPOLATION_TABLES){
                initialiseSigmaMInterpTable(Mmin,MMAX_TABLES);
                if(update){
                    initialise_dNdM_table(lnMmin, log(MMAX_TABLES), growth_out, growth_in, lnMmin, true); //WONT WORK WITH EULERIAN
                    initialise_inverse_table(lnMmin, log(MMAX_TABLES), lnMmin, growth_out, growth_in, true);
                }
                else{
                    initialise_dNdM_table(-1, Deltac, growth_out, lnMmax, lnMmin, false); //WONT WORK WITH EULERIAN
                    initialise_inverse_table(-1, Deltac, lnMmin, growth_out, 0., false);
                }
            }
        }
        //Since the conditional MF is press-schecter, we rescale by a factor equal to the ratio of the collapsed fractions (n_order == 1) of the UMF
        if(user_params->HMF!=0){
            ps_ratio = IntegratedNdM(growth_out,lnMmin,lnMmax,lnMmax,0,1,0) / IntegratedNdM(growth_out,lnMmin,lnMmax,lnMmax,0,1,user_params->HMF);
            volume = volume / ps_ratio;
        }
        else{
            ps_ratio = 1.;
        }

        if(type==0){
            //parameters for CMF
            double prefactor = RHOcrit * (1+delta_v) / sqrt(2.*PI) * cosmo_params_stoc->OMm;
            struct parameters_gsl_MF_con_int_ parameters_gsl_MF_con = {
                .redshift = z_out,
                .growthf = growth_out,
                .delta = delta_l,
                .n_order = 0,
                .M_max = lnMmax,
                .sigma_max = EvaluateSigma(lnMmax),
                .HMF = -1,
            };
            
            //using seed to select CMF or UMF since there's no RNG here
            if(seed==0){
                parameters_gsl_MF_con.HMF = user_params_stoc->HMF;
                prefactor = 1.;
                ps_ratio = 1.;
            }
            for(i=0;i<n_mass;i++){
                //conditional ps mass func * pow(M,n_order)
                if(M[i] < Mmin && seed != 0){
                    test = 0;
                }
                else{
                    test = MnMassfunction(log(M[i]),(void*)&parameters_gsl_MF_con);
                    
                    //convert to dndlnm
                    test = test * prefactor * ps_ratio;
                }
                LOG_ULTRA_DEBUG(" D %.1e | M1 %.1e | M2 %.1e | d %.1e | s %.1e -> %.1e",
                                growth_out,M[i],Mmax,delta_l,EvaluateSigma(lnMmax),test);
                result[i] = test;
            }
        }
        else if(type==1){
            //integrate CMF -> N(>M) in one condition
            //TODO: make it possible to integrate UMFs
            double M_in;
            for(i=0;i<n_mass;i++){
                M_in = M[i] < Mmin ? Mmin : M[i];
                test = IntegratedNdM(growth_out,log(M_in),lnMmax,lnMmax,delta_l,0,-1);
                //conditional MF multiplied by a few factors
                result[i] = test * volume * (1+delta_v) * (RHOcrit / sqrt(2.*PI) * cosmo_params_stoc->OMm);
            }
        }
        else if(type==2){
            //intregrate CMF -> N_halos in many condidions
            //TODO: make it possible to integrate UMFs
            //quick hack: condition gives n_order, seed ignores tables
            for(i=0;i<n_mass;i++){
                if(update){
                    R = MtoR(M[i]);
                    volume = 4. / 3. * PI * R * R * R;     
                    if(user_params_stoc->USE_INTERPOLATION_TABLES && seed == 0) test = EvaluatedNdMSpline(log(M[i]));
                    else test = IntegratedNdM(growth_out,lnMmin,log(M[i]),log(M[i]),delta_l,condition,-1);
                }
                else{
                    if(user_params_stoc->USE_INTERPOLATION_TABLES && seed == 0) test = EvaluatedNdMSpline(M[i]*growth_out);
                    else test = IntegratedNdM(growth_out,lnMmin,lnMmax,lnMmax,M[i]*growth_out,condition,-1);
                }
                //conditional MF multiplied by a few factors
                result[i] = test * volume * (1+delta_v) * (RHOcrit / sqrt(2.*PI) * cosmo_params_stoc->OMm);
            }
        }
        else if(type==3){
            //Halo mass sample from a single condition
            double *out_hm = (double *)calloc(MAX_HALO_CELL,sizeof(double));
            int n_halo;
            double param;

            if(user_params_stoc->STOC_MASS_SAMPLING){
                param = update ? growth_in : delta_l;
                //double z_out, double growth_out, double param, double M_max, double M_min, double ps_ratio, int update, gsl_rng * rng, float *M_out, int *n_halo_out
                stoc_mass_sample(z_out, growth_out, param, Mmax, Mmin, ps_ratio, update, rng_stoc[0], out_hm, &n_halo);
            }
            else{
                stoc_halo_sample(growth_out,delta_l,delta_v,volume,Mmin,Mmax,update,rng_stoc[0],&n_halo,out_hm);
            }
            //fill output array N_halo, Halomass, Stellar mass ...
            result[0] = (double)n_halo;
            int idx;
            for(idx=0;idx<n_halo;idx++){
                result[idx+1] = out_hm[idx];
            }
            free(out_hm);
        }

        //halo catalogue from a given delta distribution
        else if(type==4){
            LOG_ERROR("Not Implemented.");
            Throw(ValueError);
        }
        
        //halo catalogue from list of conditions (Mass for update, delta for !update)
        else if(type==5){
            float *halomass_out = calloc(MAX_HALO,sizeof(float));
            float *stellarmass_out = calloc(MAX_HALO,sizeof(float));
            float *sfr_out = calloc(MAX_HALO,sizeof(float));
            int *halocoords_out = calloc(MAX_HALO,3*sizeof(int));
            //need to allocate the inputs that don't matter
            int *halocoords_in = calloc(n_mass*3,sizeof(int));
            float *stellarmass_in = calloc(n_mass,sizeof(float));
            float *sfr_in = calloc(n_mass,sizeof(float));
            float *halomass_in = calloc(n_mass,sizeof(float));

            int nhalo_out;

            if(update){
                for(i=0;i<n_mass;i++){
                    halomass_in[i] = M[i];
                }
                //NOTE: using n_mass for nhalo_in
                halo_update(rng_stoc, z_in, z_out, n_mass, halocoords_in, halomass_in, stellarmass_in, sfr_in,
                                    &nhalo_out, halocoords_out, halomass_out, stellarmass_out, sfr_out);
            }
            else{
                //NOTE: Here is will be an IC density (delta(IC) = delta_l(z=0) = delta_l(z) / growth(z))
                for(i=0;i<n_mass;i++){
                    halomass_in[i] = M[i];
                }
                build_halo_cats(rng_stoc, z_out, eulerian, halomass_in, &nhalo_out, halocoords_out, halomass_out, stellarmass_out, sfr_out);
            }
            LOG_DEBUG("sampling done, %d halos, %.2e %.2e %.2e",nhalo_out,halomass_out[0],halomass_out[1],halomass_out[2]);

            result[0] = (double)nhalo_out;
            for(i=0;i<nhalo_out;i++){
                result[i+1] = halomass_out[i];
            }
            free(sfr_in);
            free(halocoords_in);
            free(stellarmass_in);

            free(halomass_out);
            free(stellarmass_out);
            free(sfr_out);
            free(halocoords_out);
        }

        //return inverse table result for M at a bunch of probabilities
        else if(type==6){
            double y_in,x_in;
            for(i=0;i<n_mass;i++){
                y_in = log(M[i]); //M are probablities here
                x_in = update ? lnMmax : delta_l;
                test = gsl_spline2d_eval(inv_NgtrM_spline,x_in,y_in,inv_NgtrM_arg_acc,inv_NgtrM_prob_acc);
                result[i] = test;
                LOG_ULTRA_DEBUG("inverse table: x = %.6e, y = %.6e p = %.6e z = %.6e",x_in,y_in,M[i],test);
            }
        }
        else{
            LOG_ERROR("Unkown output type");
            Throw(ValueError);
        }

        if(user_params_stoc->USE_INTERPOLATION_TABLES && type !=5 && type !=4){
            freeSigmaMInterpTable();
            free_dNdM_table();
            free_inverse_table();
        }
        
        free_rng_threads(rng_stoc);
    } //end of try

    Catch(status){
        return(status);
    }
    LOG_DEBUG("Done.");
    return(0);
}