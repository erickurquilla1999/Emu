#include <AMReX_MultiFabUtil.H>
#include <AMReX_PlotFileUtil.H>

#include "FlavoredNeutrinoContainer.H"
#include "IO.H"
#include "Evolve.H"

using namespace amrex;

void
WritePlotFile (const amrex::MultiFab& state,
               const FlavoredNeutrinoContainer& neutrinos,
               const amrex::Geometry& geom, amrex::Real time,
               int step, int write_plot_particles)
{
    BL_PROFILE("WritePlotFile()");

    BoxArray grids = state.boxArray();
    grids.convert(IntVect(0,0,0));

    const DistributionMapping& dmap = state.DistributionMap();

    const std::string& plotfilename = amrex::Concatenate("plt", step);

    amrex::Print() << "  Writing plotfile " << plotfilename << "\n";

    amrex::WriteSingleLevelPlotfile(plotfilename, state, GIdx::names, geom, time, step);

    if (write_plot_particles == 1)
    {
        auto neutrino_varnames = neutrinos.get_attribute_names();
        neutrinos.Checkpoint(plotfilename, "neutrinos", true, neutrino_varnames);
    }
}
