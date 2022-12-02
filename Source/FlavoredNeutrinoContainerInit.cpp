#include "FlavoredNeutrinoContainer.H"
#include "Constants.H"
#include <random>
#include <cmath>
#include <list>
#include <string>

using namespace amrex;

// generate an array of theta,phi pairs that uniformily cover the surface of a sphere
// based on DOI: 10.1080/10586458.2003.10504492 section 3.3 but specifying n_j=0 instead of n
Gpu::ManagedVector<GpuArray<Real,3> > uniform_sphere_xyz(int nphi_at_equator){
	AMREX_ASSERT(nphi_at_equator>0);
	
	Real dtheta = M_PI*std::sqrt(3)/nphi_at_equator;

	Gpu::ManagedVector<GpuArray<Real,3> > xyz;
	Real theta = 0;
	Real phi0 = 0;
	while(theta < M_PI/2.){
		int nphi = theta==0 ? nphi_at_equator : lround(nphi_at_equator * std::cos(theta));
		Real dphi = 2.*M_PI/nphi;
		if(nphi==1) theta = M_PI/2.;

		for(int iphi=0; iphi<nphi; iphi++){
			Real phi = phi0 + iphi*dphi;
			Real x = std::cos(theta) * std::cos(phi);
			Real y = std::cos(theta) * std::sin(phi);
			Real z = std::sin(theta);
			xyz.push_back(GpuArray<Real,3>{x,y,z});
			// construct exactly opposing vectors to limit subtractive cancellation errors
			// and be able to represent isotropy exactly (all odd moments == 0)
			if(theta>0) xyz.push_back(GpuArray<Real,3>{-x,-y,-z});
		}
		theta += dtheta;
		phi0 = phi0 + 0.5*dphi; // offset by half step so adjacent latitudes are not always aligned in longitude
	}

	return xyz;
}

// residual for the root finder
// Z needs to be bigger if residual is positive
// Minerbo (1978) (unfortunately under Elsevier paywall)
// Can also see Richers (2020) https://ui.adsabs.harvard.edu/abs/2020PhRvD.102h3017R
//     Eq.41 (where a is Z), but in the non-degenerate limit
//     k->0, eta->0, N->Z/(4pi sinh(Z)) (just to make it integrate to 1)
//     minerbo_residual is the "f" equation between eq.42 and 43
Real minerbo_residual(const Real fluxfac, const Real Z){
	return fluxfac - 1.0/std::tanh(Z) + 1.0 / Z;
}
Real minerbo_residual_derivative(const Real fluxfac, const Real Z){
	return 1.0/(std::sinh(Z)*std::sinh(Z)) - 1.0/(Z*Z);
}
Real minerbo_Z(const Real fluxfac){
	// hard-code in these parameters because they are not
	// really very important...
	Real maxresidual = 1e-6;
	Real maxcount = 20;
	Real minfluxfac = 1e-3;

	// set the initial conditions
	Real Z = 1.0;

	// catch the small flux factor case to prevent nans
	if(fluxfac < minfluxfac)
		Z = 3.*fluxfac;
	else{
		Real residual = 1.0;
		int count = 0;
		while(std::abs(residual)>maxresidual and count<maxcount){
			residual = minerbo_residual(fluxfac, Z);
			Real slope = minerbo_residual_derivative(fluxfac, Z);
			Z -= residual/slope;
			count++;
		}
		if(residual>maxresidual)
			amrex::Error("Failed to converge on a solution.");
	}
	
	amrex::Print() << "fluxfac="<<fluxfac<<" Z=" << Z<<std::endl;
	return Z;
}

namespace
{    
    AMREX_GPU_HOST_DEVICE void get_position_unit_cell(Real* r, const IntVect& nppc, int i_part)
    {
        int nx = nppc[0];
        int ny = nppc[1];
        int nz = nppc[2];

        int ix_part = i_part/(ny * nz);
        int iy_part = (i_part % (ny * nz)) % ny;
        int iz_part = (i_part % (ny * nz)) / ny;

        r[0] = (0.5+ix_part)/nx;
        r[1] = (0.5+iy_part)/ny;
        r[2] = (0.5+iz_part)/nz;
    }

    AMREX_GPU_HOST_DEVICE void get_random_direction(Real* u) {
        // Returns components of u normalized so |u| = 1
        // in random directions in 3D space

        Real theta = amrex::Random() * MathConst::pi;       // theta from [0, pi)
        Real phi   = amrex::Random() * 2.0 * MathConst::pi; // phi from [0, 2*pi)

        u[0] = std::sin(theta) * std::cos(phi);
        u[1] = std::sin(theta) * std::sin(phi);
        u[2] = std::cos(theta);
    }

  AMREX_GPU_HOST_DEVICE void symmetric_uniform(Real* Usymmetric){
    *Usymmetric = 2. * (amrex::Random()-0.5);
  }

// angular structure as determined by the Minerbo closure
// Z is a parameter determined by the flux factor
// mu is the cosine of the angle relative to the flux direction
// Coefficients set such that the expectation value is 1
  AMREX_GPU_HOST_DEVICE void minerbo_closure(Real* result, const Real Z, const Real mu){
	Real minfluxfac = 1e-3;
	*result = std::exp(Z*mu);
	if(Z/3.0 > minfluxfac)
		*result *= Z/std::sinh(Z);
  }
}

// angular structure as determined by the gaussian profile of Martin et al (2019).
 AMREX_GPU_HOST_DEVICE void gaussian_profile(Real* result, const Real sigma, const Real mu, const Real mu0){
   Real Ainverse = sigma * std::sqrt(M_PI/2.0) * std::erf(std::sqrt(2)/sigma);
   Real A = 1.0 / Ainverse;
   *result = 2.0 * A * std::exp(-(mu-mu0)*(mu-mu0) / (2.0*sigma*sigma));
 }

FlavoredNeutrinoContainer::
FlavoredNeutrinoContainer(const Geometry            & a_geom,
                          const DistributionMapping & a_dmap,
                          const BoxArray            & a_ba)
    : ParticleContainer<PIdx::nattribs, 0, 0, 0>(a_geom, a_dmap, a_ba)
{
    #include "generated_files/FlavoredNeutrinoContainerInit.H_particle_varnames_fill"
}

void
FlavoredNeutrinoContainer::
InitParticles(const TestParams* parms)
{
    BL_PROFILE("FlavoredNeutrinoContainer::InitParticles");

    const int lev = 0;   
    const auto dx = Geom(lev).CellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();
    const auto& a_bounds = Geom(lev).ProbDomain();
    
    const int nlocs_per_cell = AMREX_D_TERM( parms->nppc[0],
                                     *parms->nppc[1],
                                     *parms->nppc[2]);

    // array of direction vectors
    Gpu::ManagedVector<GpuArray<Real,3> > direction_vectors = uniform_sphere_xyz(parms->nphi_equator);
    auto* direction_vectors_p = direction_vectors.dataPtr();
    int ndirs_per_loc = direction_vectors.size();
    amrex::Print() << "Using " << ndirs_per_loc << " directions based on " << parms->nphi_equator << " directions at the equator." << std::endl;

    // array of random numbers, one for each grid cell
    int nrandom = parms->ncell[0] * parms->ncell[1] * parms->ncell[2];
    Gpu::ManagedVector<Real> random_numbers(nrandom);
    if (ParallelDescriptor::IOProcessor())
      for(int i=0; i<nrandom; i++)
	symmetric_uniform(&random_numbers[i]);
    auto* random_numbers_p = random_numbers.dataPtr();
    ParallelDescriptor::Bcast(random_numbers_p, random_numbers.size(),
			      ParallelDescriptor::IOProcessorNumber());

    const Real scale_fac = dx[0]*dx[1]*dx[2]/nlocs_per_cell/ndirs_per_loc;

	// get the Z parameters for the Minerbo closure if using simuation type 5
	Real Ze, Za, Zx;
	Real fluxfac_e, fluxfac_a, fluxfac_x;
	if(parms->simulation_type==5){
		fluxfac_e = std::sqrt(
			parms->st5_fxnue*parms->st5_fxnue + 
			parms->st5_fynue*parms->st5_fynue + 
			parms->st5_fznue*parms->st5_fznue );
		fluxfac_a = std::sqrt(
			parms->st5_fxnua*parms->st5_fxnua + 
			parms->st5_fynua*parms->st5_fynua + 
			parms->st5_fznua*parms->st5_fznua );
		fluxfac_x = std::sqrt(
			parms->st5_fxnux*parms->st5_fxnux + 
			parms->st5_fynux*parms->st5_fynux + 
			parms->st5_fznux*parms->st5_fznux );
		Ze = minerbo_Z(fluxfac_e);
		Za = minerbo_Z(fluxfac_a);
		Zx = minerbo_Z(fluxfac_x);
	}

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();

        const auto lo = amrex::lbound(tile_box);
        const auto hi = amrex::ubound(tile_box);

        Gpu::ManagedVector<unsigned int> counts(tile_box.numPts(), 0);
        unsigned int* pcount = counts.dataPtr();
        
        Gpu::ManagedVector<unsigned int> offsets(tile_box.numPts());
        unsigned int* poffset = offsets.dataPtr();
        
        // Determine how many particles to add to the particle tile per cell
        amrex::ParallelFor(tile_box,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            for (int i_part=0; i_part<nlocs_per_cell;i_part++)
            {
                Real r[3];
                
                get_position_unit_cell(r, parms->nppc, i_part);
                
                Real x = plo[0] + (i + r[0])*dx[0];
                Real y = plo[1] + (j + r[1])*dx[1];
                Real z = plo[2] + (k + r[2])*dx[2];
                
                if (x >= a_bounds.hi(0) || x < a_bounds.lo(0) ||
                    y >= a_bounds.hi(1) || y < a_bounds.lo(1) ||
                    z >= a_bounds.hi(2) || z < a_bounds.lo(2) ) continue;
              
                int ix = i - lo.x;
                int iy = j - lo.y;
                int iz = k - lo.z;
                int nx = hi.x-lo.x+1;
                int ny = hi.y-lo.y+1;
                int nz = hi.z-lo.z+1;            
                unsigned int uix = amrex::min(nx-1,amrex::max(0,ix));
                unsigned int uiy = amrex::min(ny-1,amrex::max(0,iy));
                unsigned int uiz = amrex::min(nz-1,amrex::max(0,iz));
                unsigned int cellid = (uix * ny + uiy) * nz + uiz;
                pcount[cellid] += ndirs_per_loc;
            }
        });

        // Determine total number of particles to add to the particle tile
        Gpu::inclusive_scan(counts.begin(), counts.end(), offsets.begin());

        int num_to_add = offsets[tile_box.numPts()-1];
        if (num_to_add == 0) continue;

        // this will be the particle ID for the first new particle in the tile
        long new_pid;
        ParticleType* pstruct;
        #ifdef _OPENMP
        #pragma omp critical
        #endif
        {

        	auto& particles = GetParticles(lev);
        	auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];

        	// Resize the particle container
        	auto old_size = particle_tile.GetArrayOfStructs().size();
        	auto new_size = old_size + num_to_add;
        	particle_tile.resize(new_size);

        	// get the next particle ID
        	new_pid = ParticleType::NextID();

        	// set the starting particle ID for the next tile of particles
        	ParticleType::NextID(new_pid + num_to_add);

        	pstruct = particle_tile.GetArrayOfStructs()().data();
        }

        int procID = ParallelDescriptor::MyProc();

	Real domain_length_z = Geom(lev).ProbLength(2);

        // Initialize particle data in the particle tile
        amrex::ParallelFor(tile_box,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            int ix = i - lo.x;
            int iy = j - lo.y;
            int iz = k - lo.z;
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            
            unsigned int uix = amrex::min(nx-1,amrex::max(0,ix));
            unsigned int uiy = amrex::min(ny-1,amrex::max(0,iy));
            unsigned int uiz = amrex::min(nz-1,amrex::max(0,iz));
            unsigned int cellid = (uix * ny + uiy) * nz + uiz;

            for (int i_loc=0; i_loc<nlocs_per_cell;i_loc++)
            {
                Real r[3];
                
                get_position_unit_cell(r, parms->nppc, i_loc);
                
                Real x = plo[0] + (i + r[0])*dx[0];
                Real y = plo[1] + (j + r[1])*dx[1];
                Real z = plo[2] + (k + r[2])*dx[2];
                
                if (x >= a_bounds.hi(0) || x < a_bounds.lo(0) ||
                    y >= a_bounds.hi(1) || y < a_bounds.lo(1) ||
                    z >= a_bounds.hi(2) || z < a_bounds.lo(2) ) continue;
                

                for(int i_direction=0; i_direction<ndirs_per_loc; i_direction++){
                    // Get the Particle data corresponding to our particle index in pidx
                    const int pidx = poffset[cellid] - poffset[0] + i_loc*ndirs_per_loc + i_direction;
                    ParticleType& p = pstruct[pidx];

                    // Set particle ID using the ID for the first of the new particles in this tile
                    // plus our zero-based particle index
                    p.id()   = new_pid + pidx;

                    // Set CPU ID
                    p.cpu()  = procID;

                    // Set particle position
                    p.pos(0) = x;
                    p.pos(1) = y;
                    p.pos(2) = z;

                    // Set particle integrated position
                    p.rdata(PIdx::x) = x;
                    p.rdata(PIdx::y) = y;
                    p.rdata(PIdx::z) = z;
                    p.rdata(PIdx::time) = 0;

                    const GpuArray<Real,3> u = direction_vectors_p[i_direction];
                    //get_random_direction(u);

		//=========================//
		// VACUUM OSCILLATION TEST //
		//=========================//
		if(parms->simulation_type==0){
		  // set all particles to start in electron state (and anti-state)
		  // Set N to be small enough that self-interaction is not important
		  // Set all particle momenta to be such that one oscillation wavelength is 1cm
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);

		  // Set particle flavor
		  p.rdata(PIdx::N) = 1.0;
		  p.rdata(PIdx::Nbar) = 1.0;
		  p.rdata(PIdx::f00_Re)    = 1.0;
		  p.rdata(PIdx::f01_Re)    = 0.0;
		  p.rdata(PIdx::f01_Im)    = 0.0;
		  p.rdata(PIdx::f11_Re)    = 0.0;
		  p.rdata(PIdx::f00_Rebar) = 1.0;
		  p.rdata(PIdx::f01_Rebar) = 0.0;
		  p.rdata(PIdx::f01_Imbar) = 0.0;
		  p.rdata(PIdx::f11_Rebar) = 0.0;

#if (NUM_FLAVORS==3)
		  p.rdata(PIdx::f22_Re)    = 0.0;
		  p.rdata(PIdx::f22_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Re)    = 0.0;
		  p.rdata(PIdx::f02_Im)    = 0.0;
		  p.rdata(PIdx::f12_Re)    = 0.0;
		  p.rdata(PIdx::f12_Im)    = 0.0;
		  p.rdata(PIdx::f02_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Imbar) = 0.0;
		  p.rdata(PIdx::f12_Rebar) = 0.0;
		  p.rdata(PIdx::f12_Imbar) = 0.0;
#endif

		  // set momentum so that a vacuum oscillation wavelength occurs over a distance of 1cm
		  // Set particle velocity to c in a random direction
		  Real dm2 = (parms->mass2-parms->mass1)*(parms->mass2-parms->mass1); //g^2
		  p.rdata(PIdx::pupt) = dm2*PhysConst::c4 * sin(2.*parms->theta12) / (8.*M_PI*PhysConst::hbarc); // *1cm for units
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);
		}

		//==========================//
		// BIPOLAR OSCILLATION TEST //
		//==========================//
		else if(parms->simulation_type==1){
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);
		  
		  // Set particle flavor
		  p.rdata(PIdx::f00_Re)    = 1.0;
		  p.rdata(PIdx::f01_Re)    = 0.0;
		  p.rdata(PIdx::f01_Im)    = 0.0;
		  p.rdata(PIdx::f11_Re)    = 0.0;
		  p.rdata(PIdx::f00_Rebar) = 1.0;
		  p.rdata(PIdx::f01_Rebar) = 0.0;
		  p.rdata(PIdx::f01_Imbar) = 0.0;
		  p.rdata(PIdx::f11_Rebar) = 0.0;

#if (NUM_FLAVORS==3)
		  p.rdata(PIdx::f22_Re)    = 0.0;
		  p.rdata(PIdx::f22_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Re)    = 0.0;
		  p.rdata(PIdx::f02_Im)    = 0.0;
		  p.rdata(PIdx::f12_Re)    = 0.0;
		  p.rdata(PIdx::f12_Im)    = 0.0;
		  p.rdata(PIdx::f02_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Imbar) = 0.0;
		  p.rdata(PIdx::f12_Rebar) = 0.0;
		  p.rdata(PIdx::f12_Imbar) = 0.0;
#endif

		  // set energy to 50 MeV to match Richers+(2019)
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);

		  // set particle weight such that density is
		  // 10 dm2 c^4 / (2 sqrt(2) GF E)
		  Real dm2 = (parms->mass2-parms->mass1)*(parms->mass2-parms->mass1); //g^2
		  double omega = dm2*PhysConst::c4 / (2.*p.rdata(PIdx::pupt));
		  double ndens = 10. * dm2*PhysConst::c4 / (2.*sqrt(2.) * PhysConst::GF * p.rdata(PIdx::pupt));
		  double mu = sqrt(2.)*PhysConst::GF * ndens;
		  p.rdata(PIdx::N) = ndens * scale_fac;
		  p.rdata(PIdx::Nbar) = ndens * scale_fac;
		}

		//========================//
		// 2-BEAM FAST FLAVOR TEST//
		//========================//
		else if(parms->simulation_type==2){
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);
		  
		  // Set particle flavor
		  p.rdata(PIdx::f00_Re)    = 1.0;
		  p.rdata(PIdx::f01_Re)    = 0.0;
		  p.rdata(PIdx::f01_Im)    = 0.0;
		  p.rdata(PIdx::f11_Re)    = 0.0;
		  p.rdata(PIdx::f00_Rebar) = 1.0;
		  p.rdata(PIdx::f01_Rebar) = 0.0;
		  p.rdata(PIdx::f01_Imbar) = 0.0;
		  p.rdata(PIdx::f11_Rebar) = 0.0;

#if (NUM_FLAVORS==3)
		  p.rdata(PIdx::f22_Re)    = 0.0;
		  p.rdata(PIdx::f22_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Re)    = 0.0;
		  p.rdata(PIdx::f02_Im)    = 0.0;
		  p.rdata(PIdx::f12_Re)    = 0.0;
		  p.rdata(PIdx::f12_Im)    = 0.0;
		  p.rdata(PIdx::f02_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Imbar) = 0.0;
		  p.rdata(PIdx::f12_Rebar) = 0.0;
		  p.rdata(PIdx::f12_Imbar) = 0.0;
#endif

		  // set energy to 50 MeV to match Richers+(2019)
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);

		  // set particle weight such that density is
		  // 0.5 dm2 c^4 / (2 sqrt(2) GF E)
		  // to get maximal growth according to Chakraborty 2016 Equation 2.10
		  Real dm2 = (parms->mass2-parms->mass1)*(parms->mass2-parms->mass1); //g^2
		  Real omega = dm2*PhysConst::c4 / (2.* p.rdata(PIdx::pupt));
		  Real mu_ndens = sqrt(2.) * PhysConst::GF; // SI potential divided by the number density
		  double ndens = omega / (2.*mu_ndens); // want omega/2mu to be 1
		  p.rdata(PIdx::N) = ndens * scale_fac * (1. + u[2]);
		  p.rdata(PIdx::Nbar) = ndens * scale_fac * (1. - u[2]);
		}

		//===============================//
		// 3- k!=0 BEAM FAST FLAVOR TEST //
		//===============================//
		else if(parms->simulation_type==3){
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);

		  // perturbation parameters
		  Real lambda = domain_length_z/(Real)parms->st3_wavelength_fraction_of_domain;
		  Real k = (2.*M_PI) / lambda;

		  // Set particle flavor
		  p.rdata(PIdx::f00_Re)    = 1.0;
		  p.rdata(PIdx::f01_Re)    = parms->st3_amplitude*sin(k*p.pos(2));
		  p.rdata(PIdx::f01_Im)    = 0.0;
		  p.rdata(PIdx::f11_Re)    = 0.0;
		  p.rdata(PIdx::f00_Rebar) = 1.0;
		  p.rdata(PIdx::f01_Rebar) = parms->st3_amplitude*sin(k*p.pos(2));
		  p.rdata(PIdx::f01_Imbar) = 0.0;
		  p.rdata(PIdx::f11_Rebar) = 0.0;

#if (NUM_FLAVORS==3) 
		  //just perturbing the electron-muon flavor state, other terms can stay = 0.0 for simplicity
		  p.rdata(PIdx::f22_Re)    = 0.0;
		  p.rdata(PIdx::f22_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Re)    = 0.0; 
		  p.rdata(PIdx::f02_Im)    = 0.0; 
		  p.rdata(PIdx::f12_Re)    = 0.0;
		  p.rdata(PIdx::f12_Im)    = 0.0;
		  p.rdata(PIdx::f02_Rebar) = 0.0; 
		  p.rdata(PIdx::f02_Imbar) = 0.0; 
		  p.rdata(PIdx::f12_Rebar) = 0.0;
		  p.rdata(PIdx::f12_Imbar) = 0.0;
#endif

		  // set energy to 50 MeV to match Richers+(2019)
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);

		  // set particle weight such that density is
		  // 0.5 dm2 c^4 / (2 sqrt(2) GF E)
		  // to get maximal growth according to Chakraborty 2016 Equation 2.10
		  Real dm2 = (parms->mass2-parms->mass1)*(parms->mass2-parms->mass1); //g^2
		  Real omega = dm2*PhysConst::c4 / (2.* p.rdata(PIdx::pupt));
		  Real mu_ndens = sqrt(2.) * PhysConst::GF; // SI potential divided by the number density
		  Real ndens = (omega+k*PhysConst::hbarc) / (2.*mu_ndens); // want omega/2mu to be 1
		  p.rdata(PIdx::N) = ndens * scale_fac * (1. + u[2]);
		  p.rdata(PIdx::Nbar) = ndens * scale_fac * (1. - u[2]);
		}

		//====================//
		// 4- k!=0 RANDOMIZED //
		//====================//
		else if(parms->simulation_type==4){
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);

		  // Set particle flavor
		  Real rand1, rand2, rand3, rand4;
		  symmetric_uniform(&rand1);
		  symmetric_uniform(&rand2);
		  symmetric_uniform(&rand3);
		  symmetric_uniform(&rand4);
		  p.rdata(PIdx::f00_Re)    = 1.0;
		  p.rdata(PIdx::f01_Re)    = parms->st4_amplitude*rand1;
		  p.rdata(PIdx::f01_Im)    = parms->st4_amplitude*rand2;
		  p.rdata(PIdx::f11_Re)    = 0.0;
		  p.rdata(PIdx::f00_Rebar) = 1.0;
		  p.rdata(PIdx::f01_Rebar) = parms->st4_amplitude*rand3;
		  p.rdata(PIdx::f01_Imbar) = parms->st4_amplitude*rand4;
		  p.rdata(PIdx::f11_Rebar) = 0.0;
#if (NUM_FLAVORS==3)
		  symmetric_uniform(&rand1);
		  symmetric_uniform(&rand2);
		  symmetric_uniform(&rand3);
		  symmetric_uniform(&rand4);
		  p.rdata(PIdx::f22_Re)    = 0.0;
		  p.rdata(PIdx::f22_Rebar) = 0.0;
		  p.rdata(PIdx::f02_Re)    = parms->st4_amplitude*rand1;
		  p.rdata(PIdx::f02_Im)    = parms->st4_amplitude*rand2;
		  p.rdata(PIdx::f12_Re)    = 0;
		  p.rdata(PIdx::f12_Im)    = 0;
		  p.rdata(PIdx::f02_Rebar) = parms->st4_amplitude*rand3;
		  p.rdata(PIdx::f02_Imbar) = parms->st4_amplitude*rand4;
		  p.rdata(PIdx::f12_Rebar) = 0;
		  p.rdata(PIdx::f12_Imbar) = 0;
#endif

		  // set energy to 50 MeV to match Richers+(2019)
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);

		  // set particle weight such that density is
		  // 0.5 dm2 c^4 / (2 sqrt(2) GF E)
		  // to get maximal growth according to Chakraborty 2016 Equation 2.10
		  //Real dm2 = (parms->mass2-parms->mass1)*(parms->mass2-parms->mass1); //g^2
		  //Real omega = dm2*PhysConst::c4 / (2.* p.rdata(PIdx::pupt));
		  //Real mu_ndens = sqrt(2.) * PhysConst::GF; // SI potential divided by the number density
		  //Real k_expected = (2.*M_PI)/1.0;// corresponding to wavelength of 1cm
		  //Real ndens_fiducial = (omega+k_expected*PhysConst::hbarc) / (2.*mu_ndens); // want omega/2mu to be 1
		  //amrex::Print() << "fiducial ndens would be " << ndens_fiducial << std::endl;
		  
		  Real ndens    = parms->st4_ndens;
		  Real ndensbar = parms->st4_ndensbar;
		  Real fhat[3]    = {cos(parms->st4_phi)   *sin(parms->st4_theta   ),
				     sin(parms->st4_phi)   *sin(parms->st4_theta   ),
				     cos(parms->st4_theta   )};
		  Real fhatbar[3] = {cos(parms->st4_phibar)*sin(parms->st4_thetabar),
				     sin(parms->st4_phibar)*sin(parms->st4_thetabar),
				     cos(parms->st4_thetabar)};
		  Real costheta    = fhat   [0]*u[0] + fhat   [1]*u[1] + fhat   [2]*u[2];
		  Real costhetabar = fhatbar[0]*u[0] + fhatbar[1]*u[1] + fhatbar[2]*u[2];
		  
		  p.rdata(PIdx::N   ) = ndens   *scale_fac * (1. + 3.*parms->st4_fluxfac   *costheta   );
		  p.rdata(PIdx::Nbar) = ndensbar*scale_fac * (1. + 3.*parms->st4_fluxfacbar*costhetabar);
		}

		//====================//
		// 5- Minerbo Closure //
		//====================//
		else if(parms->simulation_type==5){
		  AMREX_ASSERT(NUM_FLAVORS==3 or NUM_FLAVORS==2);

		  // set energy to 50 MeV
		  p.rdata(PIdx::pupt) = parms->st5_avgE_MeV * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);
		  
		  // get the cosine of the angle between the direction and each flavor's flux vector
		  Real mue = fluxfac_e>0 ? (parms->st5_fxnue*u[0] + parms->st5_fynue*u[1] + parms->st5_fznue*u[2])/fluxfac_e : 0;
		  Real mua = fluxfac_a>0 ? (parms->st5_fxnua*u[0] + parms->st5_fynua*u[1] + parms->st5_fznua*u[2])/fluxfac_a : 0;
		  Real mux = fluxfac_x>0 ? (parms->st5_fxnux*u[0] + parms->st5_fynux*u[1] + parms->st5_fznux*u[2])/fluxfac_x : 0;

		  // get the number of each flavor in this particle.
          // parms->st5_nnux contains the number density of mu+tau neutrinos+antineutrinos
		  // Nnux_thisparticle contains the number of EACH of mu/tau anti/neutrinos (hence the factor of 4)
		  Real angular_factor;
		  minerbo_closure(&angular_factor, Ze, mue);
		  Real Nnue_thisparticle = parms->st5_nnue*scale_fac * angular_factor;
		  minerbo_closure(&angular_factor, Za, mua);
		  Real Nnua_thisparticle = parms->st5_nnua*scale_fac * angular_factor;
		  minerbo_closure(&angular_factor, Zx, mux);
		  Real Nnux_thisparticle = parms->st5_nnux*scale_fac * angular_factor / 4.0;

		  // set total number of neutrinos the particle has as the sum of the flavors
		  p.rdata(PIdx::N   ) = Nnue_thisparticle + Nnux_thisparticle;
		  p.rdata(PIdx::Nbar) = Nnua_thisparticle + Nnux_thisparticle;
#if NUM_FLAVORS==3
		  p.rdata(PIdx::N   ) += Nnux_thisparticle;
		  p.rdata(PIdx::Nbar) += Nnux_thisparticle;
#endif

		  // set on-diagonals to have relative proportion of each flavor
		  p.rdata(PIdx::f00_Re)    = Nnue_thisparticle / p.rdata(PIdx::N   );
		  p.rdata(PIdx::f11_Re)    = Nnux_thisparticle / p.rdata(PIdx::N   );
		  p.rdata(PIdx::f00_Rebar) = Nnua_thisparticle / p.rdata(PIdx::Nbar);
		  p.rdata(PIdx::f11_Rebar) = Nnux_thisparticle / p.rdata(PIdx::Nbar);
#if NUM_FLAVORS==3
		  p.rdata(PIdx::f22_Re)    = Nnux_thisparticle / p.rdata(PIdx::N   );
		  p.rdata(PIdx::f22_Rebar) = Nnux_thisparticle / p.rdata(PIdx::Nbar);
#endif

		  // random perturbations to the off-diagonals
		  Real rand;
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f01_Re)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Re   ) - p.rdata(PIdx::f11_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f01_Im)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Re   ) - p.rdata(PIdx::f11_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f01_Rebar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Rebar) - p.rdata(PIdx::f11_Rebar));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f01_Imbar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Rebar) - p.rdata(PIdx::f11_Rebar));
#if NUM_FLAVORS==3
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f02_Re)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Re   ) - p.rdata(PIdx::f22_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f02_Im)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Re   ) - p.rdata(PIdx::f22_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f12_Re)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f11_Re   ) - p.rdata(PIdx::f22_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f12_Im)    = parms->st5_amplitude*rand * (p.rdata(PIdx::f11_Re   ) - p.rdata(PIdx::f22_Re   ));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f02_Rebar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Rebar) - p.rdata(PIdx::f22_Rebar));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f02_Imbar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f00_Rebar) - p.rdata(PIdx::f22_Rebar));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f12_Rebar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f11_Rebar) - p.rdata(PIdx::f22_Rebar));
		  symmetric_uniform(&rand);
		  p.rdata(PIdx::f12_Imbar) = parms->st5_amplitude*rand * (p.rdata(PIdx::f11_Rebar) - p.rdata(PIdx::f22_Rebar));
#endif
		}

		//============================//
		// 6 - Code Comparison Random //
		//============================//
		else if(parms->simulation_type==6){
		  AMREX_ASSERT(NUM_FLAVORS==2);
		  AMREX_ASSERT(parms->ncell[0] == 1);
		  AMREX_ASSERT(parms->ncell[1] == 1);

		  // set energy to 50 MeV
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);
		  
		  // get the number of each flavor in this particle.
		  Real angular_factor;
		  gaussian_profile(&angular_factor, parms->st6_sigma   , u[2], parms->st6_mu0   );
		  Real Nnue_thisparticle = parms->st6_nnue*scale_fac * angular_factor;
		  gaussian_profile(&angular_factor, parms->st6_sigmabar, u[2], parms->st6_mu0bar);
		  Real Nnua_thisparticle = parms->st6_nnua*scale_fac * angular_factor;

// 		  // set total number of neutrinos the particle has as the sum of the flavors
		  p.rdata(PIdx::N   ) = Nnue_thisparticle;
		  p.rdata(PIdx::Nbar) = Nnua_thisparticle;

// 		  // set on-diagonals to have relative proportion of each flavor
		  p.rdata(PIdx::f00_Re)    = 1;
		  p.rdata(PIdx::f11_Re)    = 0;
		  p.rdata(PIdx::f00_Rebar) = 1;
		  p.rdata(PIdx::f11_Rebar) = 0;

// 		  // random perturbations to the off-diagonals
		  p.rdata(PIdx::f01_Re) = 0;
		  p.rdata(PIdx::f01_Im) = 0;
		  int Nz = parms->ncell[2];
		  int amax = parms->st6_amax * Nz/2;
		  for(int a=-amax; a<=amax; a++){
		    if(a==0) continue;
		    Real ka = 2.*M_PI * a / parms->Lz;
		    Real phase = ka*z + 2.*M_PI*random_numbers_p[a+Nz/2];
		    Real B = parms->st6_amplitude / std::abs(float(a));
		    p.rdata(PIdx::f01_Re) += 0.5 * B * cos(phase);
		    p.rdata(PIdx::f01_Im) += 0.5 * B * sin(phase);
		  }

		  // Perturb the antineutrinos in a way that preserves the symmetries of the neutrino hamiltonian
		  p.rdata(PIdx::f01_Rebar) =  p.rdata(PIdx::f01_Re);
		  p.rdata(PIdx::f01_Imbar) = -p.rdata(PIdx::f01_Im);
		}

		//==============================//
		// 7 - Code Comparison Gaussian //
		//==============================//
		else if(parms->simulation_type==7){
		  AMREX_ASSERT(NUM_FLAVORS==2);
		  AMREX_ASSERT(parms->ncell[0] == 1);
		  AMREX_ASSERT(parms->ncell[1] == 1);

		  // set energy to 50 MeV
		  p.rdata(PIdx::pupt) = 50. * 1e6*CGSUnitsConst::eV;
		  p.rdata(PIdx::pupx) = u[0] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupy) = u[1] * p.rdata(PIdx::pupt);
		  p.rdata(PIdx::pupz) = u[2] * p.rdata(PIdx::pupt);
		  
		  // get the number of each flavor in this particle.
		  Real angular_factor;
		  gaussian_profile(&angular_factor, parms->st7_sigma   , u[2], parms->st7_mu0   );
		  Real Nnue_thisparticle = parms->st7_nnue*scale_fac * angular_factor;
		  gaussian_profile(&angular_factor, parms->st7_sigmabar, u[2], parms->st7_mu0bar);
		  Real Nnua_thisparticle = parms->st7_nnua*scale_fac * angular_factor;

// 		  // set total number of neutrinos the particle has as the sum of the flavors
		  p.rdata(PIdx::N   ) = Nnue_thisparticle;
		  p.rdata(PIdx::Nbar) = Nnua_thisparticle;

// 		  // set on-diagonals to have relative proportion of each flavor
		  p.rdata(PIdx::f00_Re)    = 1;
		  p.rdata(PIdx::f11_Re)    = 0;
		  p.rdata(PIdx::f00_Rebar) = 1;
		  p.rdata(PIdx::f11_Rebar) = 0;

// 		  // random perturbations to the off-diagonals
		  p.rdata(PIdx::f01_Re) = 0;
		  p.rdata(PIdx::f01_Im) = 0;
		  int Nz = parms->ncell[2];
		  Real zprime = z - parms->Lz;
		  Real P1 = parms->st7_amplitude * std::exp(-zprime*zprime/(2.*parms->st7_sigma_pert*parms->st7_sigma_pert));
		  p.rdata(PIdx::f01_Re) = P1 / 2.0;
		  p.rdata(PIdx::f01_Im) = 0;

		  // Perturb the antineutrinos in a way that preserves the symmetries of the neutrino hamiltonian
		  p.rdata(PIdx::f01_Rebar) =  p.rdata(PIdx::f01_Re);
		  p.rdata(PIdx::f01_Imbar) = -p.rdata(PIdx::f01_Im);
		}

		else{
            amrex::Error("Invalid simulation type");
		}

		#include "generated_files/FlavoredNeutrinoContainerInit.cpp_set_trace_length"
            }
        }
        });
    }

    // get the minimum neutrino energy for calculating the timestep
    Real pupt_min = amrex::ReduceMin(*this, [=] AMREX_GPU_HOST_DEVICE (const FlavoredNeutrinoContainer::ParticleType& p) -> Real { return p.rdata(PIdx::pupt); });
    ParallelDescriptor::ReduceRealMin(pupt_min);
    #include "generated_files/FlavoredNeutrinoContainerInit.cpp_Vvac_fill"
}

void
FlavoredNeutrinoContainer::
PerturbParticlesLyapunov(const TestParams* parms)
{
    BL_PROFILE("FlavoredNeutrinoContainer::PerturbParticles");

    const int lev = 0;

    const auto dxi = Geom(lev).InvCellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (FNParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int np  = pti.numParticles();
        ParticleType * pstruct = &(pti.GetArrayOfStructs()[0]);

        amrex::ParallelFor (np, [=] AMREX_GPU_DEVICE (int i) {
            ParticleType& p = pstruct[i];

		  // random perturbations
        #if (NUM_FLAVORS==2)
            Real rand;
            symmetric_uniform(&rand);
            double f00_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f01_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f11_Re));
            symmetric_uniform(&rand);
            double f01_Im_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f11_Re));
            symmetric_uniform(&rand);
            double f11_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f00_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f01_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f11_Rebar));
            symmetric_uniform(&rand);
            double f01_Imbar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f11_Rebar));
            symmetric_uniform(&rand);
            double f11_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand;
            p.rdata(PIdx::f00_Re)    = (p.rdata(PIdx::f00_Re)+f00_Re_Perturb)/(1.0+f00_Re_Perturb+f11_Re_Perturb);
            p.rdata(PIdx::f01_Re)    = p.rdata(PIdx::f01_Re)+f01_Re_Perturb;
            p.rdata(PIdx::f01_Im)    = p.rdata(PIdx::f01_Im)+f01_Im_Perturb;
            p.rdata(PIdx::f11_Re)    = (p.rdata(PIdx::f11_Re)+f11_Re_Perturb)/(1.0+f00_Re_Perturb+f11_Re_Perturb);
            p.rdata(PIdx::f00_Rebar) = (p.rdata(PIdx::f00_Rebar)+f00_Rebar_Perturb)/(1.0+f00_Rebar_Perturb+f11_Rebar_Perturb);
            p.rdata(PIdx::f01_Rebar) = p.rdata(PIdx::f01_Rebar)+f01_Rebar_Perturb;
            p.rdata(PIdx::f01_Imbar) = p.rdata(PIdx::f01_Imbar)+f01_Imbar_Perturb;
            p.rdata(PIdx::f11_Rebar) = (p.rdata(PIdx::f11_Rebar)+f11_Rebar_Perturb)/(1.0+f00_Rebar_Perturb+f11_Rebar_Perturb);
        #endif
        #if (NUM_FLAVORS==3)
            Real rand;
            symmetric_uniform(&rand);
            double f00_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f01_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f11_Re));
            symmetric_uniform(&rand);
            double f01_Im_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f11_Re));
            symmetric_uniform(&rand);
            double f11_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f00_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f01_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f11_Rebar));
            symmetric_uniform(&rand);
            double f01_Imbar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f11_Rebar));
            symmetric_uniform(&rand);
            double f11_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f22_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f22_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand;
            symmetric_uniform(&rand);
            double f02_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f22_Re));
            symmetric_uniform(&rand);
            double f02_Im_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Re)+p.rdata(PIdx::f22_Re));
            symmetric_uniform(&rand);
            double f12_Re_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f11_Re)+p.rdata(PIdx::f22_Re));
            symmetric_uniform(&rand);
            double f12_Im_Perturb    = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f11_Re)+p.rdata(PIdx::f22_Re));
            symmetric_uniform(&rand);
            double f02_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f22_Rebar));
            symmetric_uniform(&rand);
            double f02_Imbar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f00_Rebar)+p.rdata(PIdx::f22_Rebar));
            symmetric_uniform(&rand);
            double f12_Rebar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f11_Rebar)+p.rdata(PIdx::f22_Rebar));
            symmetric_uniform(&rand);
            double f12_Imbar_Perturb = parms->Perturbation_Amplitud_Lyapunov*rand*(p.rdata(PIdx::f11_Rebar)+p.rdata(PIdx::f22_Rebar));
            p.rdata(PIdx::f00_Re)    = (p.rdata(PIdx::f00_Re)+f00_Re_Perturb)/(1.0+f00_Re_Perturb+f11_Re_Perturb+f22_Re_Perturb);
            p.rdata(PIdx::f01_Re)    = p.rdata(PIdx::f01_Re)+f01_Re_Perturb;
            p.rdata(PIdx::f01_Im)    = p.rdata(PIdx::f01_Im)+f01_Im_Perturb;
            p.rdata(PIdx::f11_Re)    = (p.rdata(PIdx::f11_Re)+f11_Re_Perturb)/(1.0+f00_Re_Perturb+f11_Re_Perturb+f22_Re_Perturb);
            p.rdata(PIdx::f00_Rebar) = (p.rdata(PIdx::f00_Rebar)+f00_Rebar_Perturb)/(1.0+f00_Rebar_Perturb+f11_Rebar_Perturb+f22_Rebar_Perturb);
            p.rdata(PIdx::f01_Rebar) = p.rdata(PIdx::f01_Rebar)+f01_Rebar_Perturb;
            p.rdata(PIdx::f01_Imbar) = p.rdata(PIdx::f01_Imbar)+f01_Imbar_Perturb;
            p.rdata(PIdx::f11_Rebar) = (p.rdata(PIdx::f11_Rebar)+f11_Rebar_Perturb)/(1.0+f00_Rebar_Perturb+f11_Rebar_Perturb+f22_Rebar_Perturb);
            p.rdata(PIdx::f22_Re)    = (p.rdata(PIdx::f22_Re)+f22_Re_Perturb)/(1.0+f00_Re_Perturb+f11_Re_Perturb+f22_Re_Perturb);
            p.rdata(PIdx::f22_Rebar) = (p.rdata(PIdx::f22_Rebar)+f22_Rebar_Perturb)/(1.0+f00_Rebar_Perturb+f11_Rebar_Perturb+f22_Rebar_Perturb);
            p.rdata(PIdx::f02_Re)    = p.rdata(PIdx::f02_Re)+f02_Re_Perturb;
            p.rdata(PIdx::f02_Im)    = p.rdata(PIdx::f02_Im)+f02_Im_Perturb;
            p.rdata(PIdx::f12_Re)    = p.rdata(PIdx::f12_Re)+f12_Re_Perturb;
            p.rdata(PIdx::f12_Im)    = p.rdata(PIdx::f12_Im)+f12_Im_Perturb;
            p.rdata(PIdx::f02_Rebar) = p.rdata(PIdx::f02_Rebar)+f02_Rebar_Perturb;
            p.rdata(PIdx::f02_Imbar) = p.rdata(PIdx::f02_Imbar)+f02_Imbar_Perturb;
            p.rdata(PIdx::f12_Rebar) = p.rdata(PIdx::f12_Rebar)+f12_Rebar_Perturb;
            p.rdata(PIdx::f12_Imbar) = p.rdata(PIdx::f12_Imbar)+f12_Imbar_Perturb;
        #endif
        });
    }
}

double
FlavoredNeutrinoContainer::
ComputeStateSpaceDifferenceLyapunov(const TestParams* parms,FlavoredNeutrinoContainer& given)
{
    BL_PROFILE("FlavoredNeutrinoContainer::Compute_State_Space_Diff");

    const int lev = 0;

    const auto dxi = Geom(lev).InvCellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();
	
	double sum_particles=0;

	FNParIter pti1(*this, lev);
	
	for (pti1; pti1.isValid(); ++pti1){

		// finding matching pti

		ParticleType * pstruct1 = &(pti1.GetArrayOfStructs()[0]);
		ParticleType& p1 = pstruct1[0];

		FNParIter pti2(given, lev);
		
		int pti_found=0;

		for (pti2; pti2.isValid(); ++pti2){

			const int np2  = pti2.numParticles();
			ParticleType * pstruct2 = &(pti2.GetArrayOfStructs()[0]);
			
			for (int j = 0; j < np2; j++){

				ParticleType& p2 = pstruct2[j];

				if (p1.rdata(PIdx::x)==p2.rdata(PIdx::x) && p1.rdata(PIdx::y)==p2.rdata(PIdx::y) && p1.rdata(PIdx::z)==p2.rdata(PIdx::z) && p1.rdata(PIdx::time)==p2.rdata(PIdx::time) && p1.rdata(PIdx::pupx)==p2.rdata(PIdx::pupx) && p1.rdata(PIdx::pupy)==p2.rdata(PIdx::pupy) && p1.rdata(PIdx::pupz)==p2.rdata(PIdx::pupz) ){
					pti_found=1;
					break;
				}	
			}

			if (pti_found){
				break;
			}
		}

		// computing the magnitud of diference of the state space vector 

		const int np1  = pti1.numParticles();
		const int np2  = pti2.numParticles();
		ParticleType * pstruct2 = &(pti2.GetArrayOfStructs()[0]);

		for (int i = 0; i < np1; i++){
				
			ParticleType& p1 = pstruct1[i];

			int par_found=0;

			for (int j = 0; j < np2; j++){
			
				ParticleType& p2 = pstruct2[j];

				if (p1.rdata(PIdx::x)==p2.rdata(PIdx::x) && p1.rdata(PIdx::y)==p2.rdata(PIdx::y) && p1.rdata(PIdx::z)==p2.rdata(PIdx::z) && p1.rdata(PIdx::time)==p2.rdata(PIdx::time) && p1.rdata(PIdx::pupx)==p2.rdata(PIdx::pupx) && p1.rdata(PIdx::pupy)==p2.rdata(PIdx::pupy) && p1.rdata(PIdx::pupz)==p2.rdata(PIdx::pupz) ){
					
					par_found=1;

					sum_particles += pow((p1.rdata(PIdx::f00_Re)-p2.rdata(PIdx::f00_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f01_Re)-p2.rdata(PIdx::f01_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f01_Im)-p2.rdata(PIdx::f01_Im)),2);
					sum_particles += pow((p1.rdata(PIdx::f02_Re)-p2.rdata(PIdx::f02_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f02_Im)-p2.rdata(PIdx::f02_Im)),2);
					sum_particles += pow((p1.rdata(PIdx::f11_Re)-p2.rdata(PIdx::f11_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f12_Re)-p2.rdata(PIdx::f12_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f12_Im)-p2.rdata(PIdx::f12_Im)),2);
					sum_particles += pow((p1.rdata(PIdx::f22_Re)-p2.rdata(PIdx::f22_Re)),2);
					sum_particles += pow((p1.rdata(PIdx::f00_Rebar)-p2.rdata(PIdx::f00_Rebar)),2);
					sum_particles += pow((p1.rdata(PIdx::f01_Rebar)-p2.rdata(PIdx::f01_Rebar)),2);
					sum_particles += pow((p1.rdata(PIdx::f01_Imbar)-p2.rdata(PIdx::f01_Imbar)),2);
					sum_particles += pow((p1.rdata(PIdx::f02_Rebar)-p2.rdata(PIdx::f02_Rebar)),2);
					sum_particles += pow((p1.rdata(PIdx::f02_Imbar)-p2.rdata(PIdx::f02_Imbar)),2);
					sum_particles += pow((p1.rdata(PIdx::f11_Rebar)-p2.rdata(PIdx::f11_Rebar)),2);
					sum_particles += pow((p1.rdata(PIdx::f12_Rebar)-p2.rdata(PIdx::f12_Rebar)),2);
					sum_particles += pow((p1.rdata(PIdx::f12_Imbar)-p2.rdata(PIdx::f12_Imbar)),2);
					sum_particles += pow((p1.rdata(PIdx::f22_Rebar)-p2.rdata(PIdx::f22_Rebar)),2);
					
					break;
				}

			}

			if (par_found==0){
				amrex::Print() <<"error: particle not found"<< std::endl;
			}
		}
	}
	return pow(sum_particles,0.5);
}















void 
FlavoredNeutrinoContainer::
RenormalizePerturbationLyapunov(const TestParams* parms,FlavoredNeutrinoContainer& given, double ss_vector_diff)
{
    BL_PROFILE("FlavoredNeutrinoContainer::Restart_Perturbation");

	if (ss_vector_diff!=0.0){

		const int lev = 0;

		const auto dxi = Geom(lev).InvCellSizeArray();
		const auto plo = Geom(lev).ProbLoArray();
		
		FNParIter pti1(*this, lev);
		
		for (pti1; pti1.isValid(); ++pti1){

			// finding matching pti

			ParticleType * pstruct1 = &(pti1.GetArrayOfStructs()[0]);
			ParticleType& p1 = pstruct1[0];

			FNParIter pti2(given, lev);
			
			int pti_found=0;

			for (pti2; pti2.isValid(); ++pti2){

				const int np2  = pti2.numParticles();
				ParticleType * pstruct2 = &(pti2.GetArrayOfStructs()[0]);
				
				for (int j = 0; j < np2; j++){

					ParticleType& p2 = pstruct2[j];

					if (p1.rdata(PIdx::x)==p2.rdata(PIdx::x) && p1.rdata(PIdx::y)==p2.rdata(PIdx::y) && p1.rdata(PIdx::z)==p2.rdata(PIdx::z) && p1.rdata(PIdx::time)==p2.rdata(PIdx::time) && p1.rdata(PIdx::pupx)==p2.rdata(PIdx::pupx) && p1.rdata(PIdx::pupy)==p2.rdata(PIdx::pupy) && p1.rdata(PIdx::pupz)==p2.rdata(PIdx::pupz) ){
						pti_found=1;
						break;
					}	
				}

				if (pti_found){
					break;
				}
			}

			// renormalizing the state space vector 

			const int np1  = pti1.numParticles();
			const int np2  = pti2.numParticles();
			ParticleType * pstruct2 = &(pti2.GetArrayOfStructs()[0]);

			for (int i = 0; i < np1; i++){
					
				ParticleType& p1 = pstruct1[i];

				int par_found=0;

				for (int j = 0; j < np2; j++){
				
					ParticleType& p2 = pstruct2[j];

					if (p1.rdata(PIdx::x)==p2.rdata(PIdx::x) && p1.rdata(PIdx::y)==p2.rdata(PIdx::y) && p1.rdata(PIdx::z)==p2.rdata(PIdx::z) && p1.rdata(PIdx::time)==p2.rdata(PIdx::time) && p1.rdata(PIdx::pupx)==p2.rdata(PIdx::pupx) && p1.rdata(PIdx::pupy)==p2.rdata(PIdx::pupy) && p1.rdata(PIdx::pupz)==p2.rdata(PIdx::pupz) ){

						par_found=1;

						p1.rdata(PIdx::f00_Re) = p2.rdata(PIdx::f00_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f00_Re)-p2.rdata(PIdx::f00_Re))/ss_vector_diff;
						p1.rdata(PIdx::f01_Re) = p2.rdata(PIdx::f01_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f01_Re)-p2.rdata(PIdx::f01_Re))/ss_vector_diff;
						p1.rdata(PIdx::f01_Im) = p2.rdata(PIdx::f01_Im)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f01_Im)-p2.rdata(PIdx::f01_Im))/ss_vector_diff;
						p1.rdata(PIdx::f02_Re) = p2.rdata(PIdx::f02_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f02_Re)-p2.rdata(PIdx::f02_Re))/ss_vector_diff;
						p1.rdata(PIdx::f02_Im) = p2.rdata(PIdx::f02_Im)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f02_Im)-p2.rdata(PIdx::f02_Im))/ss_vector_diff;
						p1.rdata(PIdx::f11_Re) = p2.rdata(PIdx::f11_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f11_Re)-p2.rdata(PIdx::f11_Re))/ss_vector_diff;
						p1.rdata(PIdx::f12_Re) = p2.rdata(PIdx::f12_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f12_Re)-p2.rdata(PIdx::f12_Re))/ss_vector_diff;
						p1.rdata(PIdx::f12_Im) = p2.rdata(PIdx::f12_Im)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f12_Im)-p2.rdata(PIdx::f12_Im))/ss_vector_diff;
						p1.rdata(PIdx::f22_Re) = p2.rdata(PIdx::f22_Re)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f22_Re)-p2.rdata(PIdx::f22_Re))/ss_vector_diff;
						p1.rdata(PIdx::f00_Rebar) = p2.rdata(PIdx::f00_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f00_Rebar)-p2.rdata(PIdx::f00_Rebar))/ss_vector_diff;
						p1.rdata(PIdx::f01_Rebar) = p2.rdata(PIdx::f01_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f01_Rebar)-p2.rdata(PIdx::f01_Rebar))/ss_vector_diff;
						p1.rdata(PIdx::f01_Imbar) = p2.rdata(PIdx::f01_Imbar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f01_Imbar)-p2.rdata(PIdx::f01_Imbar))/ss_vector_diff;
						p1.rdata(PIdx::f02_Rebar) = p2.rdata(PIdx::f02_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f02_Rebar)-p2.rdata(PIdx::f02_Rebar))/ss_vector_diff;
						p1.rdata(PIdx::f02_Imbar) = p2.rdata(PIdx::f02_Imbar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f02_Imbar)-p2.rdata(PIdx::f02_Imbar))/ss_vector_diff;
						p1.rdata(PIdx::f11_Rebar) = p2.rdata(PIdx::f11_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f11_Rebar)-p2.rdata(PIdx::f11_Rebar))/ss_vector_diff;
						p1.rdata(PIdx::f12_Rebar) = p2.rdata(PIdx::f12_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f12_Rebar)-p2.rdata(PIdx::f12_Rebar))/ss_vector_diff;
						p1.rdata(PIdx::f12_Imbar) = p2.rdata(PIdx::f12_Imbar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f12_Imbar)-p2.rdata(PIdx::f12_Imbar))/ss_vector_diff;
						p1.rdata(PIdx::f22_Rebar) = p2.rdata(PIdx::f22_Rebar)+parms->Perturbation_Amplitud_Lyapunov*(p1.rdata(PIdx::f22_Rebar)-p2.rdata(PIdx::f22_Rebar))/ss_vector_diff;

						double traza=p1.rdata(PIdx::f00_Re)+p1.rdata(PIdx::f11_Re)+p1.rdata(PIdx::f22_Re);
						if (traza<0.99 || traza>1.01){
							amrex::Print() << "traza error: " << traza << std::endl;
						}
						
						double trazabar=p1.rdata(PIdx::f00_Rebar)+p1.rdata(PIdx::f11_Rebar)+p1.rdata(PIdx::f22_Rebar);
						if (trazabar<0.99 || trazabar>1.01){
							amrex::Print() << "trazabar error: " << trazabar << std::endl;
						}

						break;
					}
				}

				if (par_found==0){
					amrex::Print() <<"error: particle not found"<< std::endl;
				}

			}
		}
	}else{
		amrex::Print() <<"error: ss_vector_diff=0, the renormalization of the perturbation was not posible"<< std::endl;
	}
}
