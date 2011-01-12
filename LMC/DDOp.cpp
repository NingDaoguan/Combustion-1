#include <winstd.H>

#include "DDOp.H"
#include "DDOp_F.H"
#include "LO_F.H"

#include <iostream>
using std::cout;
using std::endl;

#include <fstream>
#include "BoxLib_Data_Dump.H"


// Note: The ratio between the original grids and the coarse-fine boundary data
//       registers can be anything, but the ratio between adjacent DD operators
//       generated for multigrid will always be two.
const IntVect MGIV = IntVect(D_DECL(2,2,2));
DDOp::DD_Model DDOp::transport_model = DDOp::DD_Model_NumModels; // ...must set prior to first ctr call
ChemDriver* DDOp::chem = 0; // ...must set prior to first ctr call
int DDOp::maxorder = 3;
int DDOp::mgLevelsMAX = -1;
int DDOp::mgLevels = 0;

DDOp::DDOp ()
    : coarser(0)
{
}

DDOp::DDOp (const BoxArray&   grids,
            const Box&        box,
            const IntVect&    ratio,
            int               mgLevel)
    : coarser(0)
{
    define(grids,box,ratio,mgLevel);
}

DDOp::~DDOp ()
{
    if (coarser)
        delete coarser;
}

const DDBndry&
DDOp::TBndry(int level) const
{
    BL_ASSERT(level >= 0);
    if (level > 0)
    {
        return coarser->TBndry(level-1);
    }
    return Tbd;
}

const DDBndry&
DDOp::YBndry(int level) const
{
    BL_ASSERT(level >= 0);
    if (level > 0)
    {
        return coarser->YBndry(level-1);
    }
    return Ybd;
}

const Box&
DDOp::domain(int level) const
{
    BL_ASSERT(level >= 0);
    if (level > 0)
    {
        return coarser->domain(level-1);
    }
    BL_ASSERT(Tbd.getGeom().Domain()==Ybd.getGeom().Domain());
    return Tbd.getGeom().Domain();
}

const BoxArray&
DDOp::boxArray(int level) const
{
    BL_ASSERT(level >= 0);
    if (level > 0)
    {
        return coarser->boxArray(level-1);
    }
    return grids;
}

void
DDOp::ensure_valid_transport_is_set() const
{
    std::string id;
    if (transport_model==DD_Model_Full)
    {
        id = "DD_Model_Full";
    }
    else if (transport_model==DD_Model_MixAvg) 
    {
        id = "DD_Model_MixAvg";
    }
    else
    {
        BoxLib::Abort("Must set the static DDOp::transport_model");
    }
    if (ParallelDescriptor::IOProcessor()) {
        std::cout << "DDOp transport model: " << id << std::endl;
    }
}

bool can_coarsen(const BoxArray& ba)
{
    // Ratio between levels here will always be MGIV
    for (int i = 0; i < ba.size(); ++i)
    {
        Box tmp = ba[i];
        Box ctmp  = BoxLib::coarsen(ba[i],MGIV);
        Box rctmp = BoxLib::refine(ctmp,MGIV);
        if (tmp != rctmp || ctmp.numPts() == 1)
            return false;
    }
    return true;
}

void
DDOp::define (const BoxArray& _grids,
              const Box&      box,
              const IntVect&  ratio,
              int             mgLevel)
{
    if (!chem) {
        BoxLib::Abort("ChemDriver must be set prior to first DDOp ctr call");
    }
    if (mgLevel==0) {
        ensure_valid_transport_is_set();
    }
    grids = _grids;
    Geometry geom(box);
    int Nspec = chem->numSpecies();
    Tbd.define(grids,1,geom,mgLevel);
    Ybd.define(grids,Nspec,geom,mgLevel);
    cfRatio = ratio;
    const int gGrow = 0;
    geom.GetVolume(volInv,grids,gGrow);
    for (MFIter mfi(volInv); mfi.isValid(); ++mfi) {
        volInv[mfi].invert(1);
    }
    area.resize(BL_SPACEDIM,PArrayManage);
    for (int dir = 0; dir < BL_SPACEDIM; dir++) {
        area.set(dir,new MultiFab());
        geom.GetFaceArea(area[dir],grids,dir,gGrow);
    }
    /*  Not yet...
    flux.resize(BL_SPACEDIM,PArrayManage);
    for (int dir = 0; dir < BL_SPACEDIM; dir++) {
        BoxArray eba=BoxArray(grids).surroundingNodes(dir);
        flux.set(dir,new MultiFab(eba,Nspec+1,0));
    }
    */
    int model_DD0_MA1 = (transport_model==DD_Model_Full ? 0 : 1);
    int nComp = FORT_DDNCOEFS(model_DD0_MA1);
    int nGrow = 1;
    coefs.define(grids,nComp,nGrow,Fab_allocate);

    // Weights used to generate grow cell data
    stencilWeight.setBoxes(grids);
    for (OrientationIter face; face; ++face)
    {
        int in_rad_st = 1;
        int out_rad_st = 0;
        int extent_rad_st = 0;
        int ncomp_st = 2;
        stencilWeight.define(face(),IndexType::TheCellType(),in_rad_st,out_rad_st,extent_rad_st,ncomp_st);
    }

    // Generate coarser one (ratio = MGIV), if possible
    if ( (mgLevelsMAX<0  ||  mgLevel+1<mgLevelsMAX)  &&  can_coarsen(grids))
    {
        const BoxArray cGrids = BoxArray(grids).coarsen(MGIV);
        const Box cBox = Box(box).coarsen(MGIV);
        int mgLevelC = mgLevel + 1;
        mgLevels = mgLevelC + 1;
        BL_ASSERT(Box(cBox).refine(MGIV) == box);
        coarser = new DDOp(cGrids,cBox,cfRatio,mgLevelC);
    }
}

void
DDOp::center_to_edge (const FArrayBox& cfab,
                      FArrayBox&       efab,
                      const Box&       ccBox,
                      int              sComp,
                      int              dComp,
                      int              nComp) const
{
    // Compute data on edges between each pair of cc values in the dir direction
    const Box&      ebox = efab.box();
    const IndexType ixt  = ebox.ixType();

    BL_ASSERT(!(ixt.cellCentered()) && !(ixt.nodeCentered()));

    int dir = -1;
    for (int d = 0; d < BL_SPACEDIM; d++)
        if (ixt.test(d))
            dir = d;

    BL_ASSERT(BoxLib::grow(ccBox,-BoxLib::BASISV(dir)).contains(BoxLib::enclosedCells(ebox)));
    BL_ASSERT(sComp+nComp <= cfab.nComp() && dComp+nComp <= efab.nComp());

    FORT_DDC2E(ccBox.loVect(),ccBox.hiVect(),
               cfab.dataPtr(sComp),ARLIM(cfab.loVect()),ARLIM(cfab.hiVect()),
               efab.dataPtr(dComp),ARLIM(efab.loVect()),ARLIM(efab.hiVect()),
               &nComp, &dir);
}    

void
coarsenBndryData(const MultiFab&      fmf,
                 int                  fmf_sc,
                 const BndryRegister* fbr,
                 int                  fbr_sc,
                 MultiFab&            cmf,
                 int                  cmf_sc,
                 BndryRegister*       cbr,
                 int                  cbr_sc,
                 int                  nc)
{
    // coarsen boundary data, always by MGIV
    BL_ASSERT(fmf.boxArray() == BoxArray(cmf.boxArray()).refine(MGIV));
    for (MFIter mfi(fmf); mfi.isValid(); ++mfi)
    {
        const FArrayBox& ffab = fmf[mfi];
        FArrayBox& cfab = cmf[mfi];
        const Box& gbox = mfi.validbox();
        for (OrientationIter oitr; oitr; ++oitr)
        {
            const Box fbox = BoxLib::adjCell(gbox,oitr(),1);
            const int face = (int)oitr();
            FORT_CRSNCCBND(fbox.loVect(), fbox.hiVect(),
                           ffab.dataPtr(fmf_sc), ARLIM(ffab.loVect()), ARLIM(ffab.hiVect()),
                           cfab.dataPtr(cmf_sc), ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
                           &nc, &face, MGIV.getVect());
        }
    }

    if (fbr && cbr)
    {
        const BoxArray& fgrids = fbr->boxes();
        const BoxArray& cgrids = cbr->boxes();
        BL_ASSERT(fgrids == BoxArray(cgrids).refine(MGIV));
        for (OrientationIter oitr; oitr; ++oitr)
        {
            const FabSet& ffs = (*fbr)[oitr()];
            for (FabSetIter ffsi(ffs); ffsi.isValid(); ++ffsi)
            {
                const Box fbox = BoxLib::adjCell(fgrids[ffsi.index()],oitr(),1);
                const int face = (int)oitr();
                const FArrayBox& ffab = ffs[ffsi];
                FArrayBox& cfab = (*cbr)[oitr()][ffsi];
                FORT_CRSNCCBND(fbox.loVect(), fbox.hiVect(),
                               ffab.dataPtr(fbr_sc), ARLIM(ffab.loVect()), ARLIM(ffab.hiVect()),
                               cfab.dataPtr(cbr_sc), ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
                               &nc, &face, MGIV.getVect());
            }
        }
    }
}                 

void
DDOp::setBoundaryData(const MultiFab&      fineT,
                      int                  fStartT,
                      const MultiFab&      fineY,
                      int                  fStartY,
                      const BndryRegister* cbrT,
                      int                  cStartT,
                      const BndryRegister* cbrY,
                      int                  cStartY,
                      const BCRec&         bcT,
                      const BCRec&         bcY)
{
    BL_ASSERT(fineT.boxArray() == grids);
    BL_ASSERT(fineY.boxArray() == grids);
    const int Nspec = chem->numSpecies();

    if (coarser)  // if so, then it will be MGIV coarser
    {
        const int newTmfComp = Nspec;
        const int newTbrComp = Nspec;
        const int newYmfComp = 0;
        const int newYbrComp = 0;
        const BoxArray cBA = BoxArray(grids).coarsen(MGIV);
        MultiFab newMF(cBA,Nspec+1,1);
#ifndef NDEBUG
        newMF.setVal(-1.0);
#endif
        BoxArray ccBA = BoxArray(cBA).coarsen(MGIV);
        BndryRegister* newBR = 0;
        if (cbrT && cbrY && coarser->coarser)
        {
            newBR = new BndryRegister();
            newBR->setBoxes(ccBA);
            for (OrientationIter fi; fi; ++fi)
                newBR->define(fi(),IndexType::TheCellType(),0,1,1,Nspec+1);
#ifndef NDEBUG
            newBR->setVal(-1.0);
#endif
        }
        coarsenBndryData(fineT,fStartT,cbrT,cStartT,
                         newMF,newTmfComp,newBR,newTbrComp,1);
        coarsenBndryData(fineY,fStartY,cbrY,cStartY,
                         newMF,newYmfComp,newBR,newYbrComp,Nspec);
        coarser->setBoundaryData(newMF,newTmfComp,newMF,newYmfComp,
                                 newBR,newTbrComp,newBR,newYbrComp,bcT,bcY);
    }

    IntVect ratio(cfRatio); // To avoid const problems
    if (cbrT == 0)
    {
        Tbd.setBndryValues(fineT,fStartT,0,1,bcT);
    }
    else
    {
        Tbd.setBndryValues(const_cast<BndryRegister&>(*cbrT),
                           cStartT,fineT,fStartT,0,1,ratio,bcT);
    }

    if (cbrY == 0)
    {
        Ybd.setBndryValues(fineY,fStartY,0,Nspec,bcY);
    }
    else
    {
        Ybd.setBndryValues(const_cast<BndryRegister&>(*cbrY),
                           cStartY,fineY,fStartY,0,Nspec,ratio,bcY);
    }
}

void
DDOp::setGrowCells(MultiFab& YT,
                   int       compT,
                   int       compY)
{
    BL_ASSERT(YT.nGrow() >= 1);
    const int Nspec = chem->numSpecies();

    BL_ASSERT(YT.nComp() > compT);
    BL_ASSERT(YT.nComp() >= compY + Nspec);

    BL_ASSERT(YT.boxArray() == grids);

    YT.FillBoundary(compT,1);
    Tbd.getGeom().FillPeriodicBoundary(YT,compT,1);
    YT.FillBoundary(compY,Nspec);
    Ybd.getGeom().FillPeriodicBoundary(YT,compY,Nspec);

    const int flagbc  = 1;
    const int flagden = 1;
    const Real* dx = Tbd.getGeom().CellSize();
    for (OrientationIter oitr; oitr; ++oitr)
    {
        const Orientation&      face = oitr();
        const int              iFace = (int)face;

        const Array<Array<BoundCond> >& Tbc = Tbd.bndryConds(face);
        const Array<Real>&      Tloc = Tbd.bndryLocs(face);
        const FabSet&            Tfs = Tbd.bndryValues(face);
        FabSet&                  Tst = stencilWeight[face];
        const int                Tnc = 1;
        
        const Array<Array<BoundCond> >&  Ybc = Ybd.bndryConds(face);
        const Array<Real>&      Yloc = Ybd.bndryLocs(face);
        const FabSet&            Yfs = Ybd.bndryValues(face);
        FabSet&                  Yst = stencilWeight[face];
        const int                Ync = Nspec;

        const int comp = 0;
        for (MFIter mfi(YT); mfi.isValid(); ++mfi)
        {
            const int   idx = mfi.index();
            const Box& vbox = mfi.validbox();
            BL_ASSERT(grids[idx] == vbox);

            FArrayBox& Tfab = YT[mfi];
            FArrayBox& Tstfab = Tst[mfi];
            const FArrayBox& Tb = Tfs[mfi];

            const Mask& Tm  = Tbd.bndryMasks(face)[idx];
            const Real Tbcl = Tloc[idx];
            const int Tbct  = Tbc[idx][comp];

            FORT_APPLYBC(&flagden, &flagbc, &maxorder,
                         Tfab.dataPtr(compT), ARLIM(Tfab.loVect()), ARLIM(Tfab.hiVect()),
                         &iFace, &Tbct, &Tbcl,
                         Tb.dataPtr(), ARLIM(Tb.loVect()), ARLIM(Tb.hiVect()),
                         Tm.dataPtr(), ARLIM(Tm.loVect()), ARLIM(Tm.hiVect()),
                         Tstfab.dataPtr(1), ARLIM(Tstfab.loVect()), ARLIM(Tstfab.hiVect()),
                         vbox.loVect(),vbox.hiVect(), &Tnc, dx);

            FArrayBox& Yfab = YT[mfi];
            FArrayBox& Ystfab = Yst[mfi];
            const FArrayBox& Yb = Yfs[mfi];

            const Mask& Ym  = Ybd.bndryMasks(face)[idx];
            const Real Ybcl = Yloc[idx];
            const int Ybct  = Ybc[idx][comp];

            FORT_APPLYBC(&flagden, &flagbc, &maxorder,
                         Yfab.dataPtr(compY), ARLIM(Yfab.loVect()), ARLIM(Yfab.hiVect()),
                         &iFace, &Ybct, &Ybcl,
                         Yb.dataPtr(), ARLIM(Yb.loVect()), ARLIM(Yb.hiVect()),
                         Ym.dataPtr(), ARLIM(Ym.loVect()), ARLIM(Ym.hiVect()),
                         Ystfab.dataPtr(0), ARLIM(Ystfab.loVect()), ARLIM(Ystfab.hiVect()),
                         vbox.loVect(),vbox.hiVect(), &Ync, dx);
        }
    }
}

void
DDOp::setCoefficients(const MFIter&    mfi,
                      const FArrayBox& inYT,
                      const FArrayBox& cpi)
{
    int nGrow = 1;
    int Nspec = chem->numSpecies();
    BL_ASSERT(inYT.nComp()>=Nspec+1);

    const Box gbox = Box(grids[mfi.index()]).grow(nGrow);
    BL_ASSERT(inYT.box().contains(gbox));
    BL_ASSERT(cpi.box().contains(gbox));

    int Full0_Mix1 = (transport_model == DD_Model_Full ? 0 : 1);
    FORT_DDCOEFS(gbox.loVect(), gbox.hiVect(),
                 coefs[mfi].dataPtr(), ARLIM(coefs[mfi].loVect()), ARLIM(coefs[mfi].hiVect()),
                 inYT.dataPtr(), ARLIM(inYT.loVect()),  ARLIM(inYT.hiVect()),
                 cpi.dataPtr(), ARLIM(cpi.loVect()),  ARLIM(cpi.hiVect()),
                 Full0_Mix1);
}

void
DDOp::applyOp(MultiFab&         outYH,
              const MultiFab&   inYT,
              PArray<MultiFab>& fluxYH,
              DD_ApForTorRH     whichApp,
              bool              updateCoefs,
              int               level,
              bool              getAlpha,
              MultiFab*         alpha)
{
    BL_ASSERT(level >= 0);
    if (level > 0)
    {
        coarser->applyOp(outYH,inYT,fluxYH,whichApp,updateCoefs,level-1,getAlpha,alpha);
        return;
    }

    const int Nspec = chem->numSpecies();
    const int nGrow = 1;

    int nc = Nspec+1;
    BL_ASSERT(outYH.nComp() >= nc);
    BL_ASSERT(inYT.nComp() >= nc);
    BL_ASSERT(outYH.boxArray() == grids);
    BL_ASSERT(inYT.boxArray() == grids);
    BL_ASSERT(inYT.nGrow() >= nGrow);

    // Need grow cells in X,T to compute forcing
    // Promise to change only the grow cells in T,Y
    int sCompY = 0;
    int sCompT = Nspec;
    setGrowCells(const_cast<MultiFab&>(inYT),sCompT,sCompY);

    // Initialize output
    outYH.setVal(0,0,nc);
    for (int d=0; d<BL_SPACEDIM; ++d) {
        fluxYH[d].setVal(0,0,nc);
    }

    const IntVect iv=IntVect::TheZeroVector();
    FArrayBox dum(Box(iv,iv),1);
    if (getAlpha) {
        BL_ASSERT(alpha->nGrow()>=1);
        alpha->setVal(0);
    }

    int for_T0_H1 = (whichApp==DD_Temp ? 0 : 1);
    int Full0_Mix1 = (transport_model == DD_Model_Full ? 0 : 1);

    FArrayBox FcpDTc, CPic, Xc;
    FArrayBox Hic(Box(iv,iv),1);
    FArrayBox FcpDTe(Box(iv,iv),1);
    const Real* dx = Tbd.getGeom().CellSize();
    if (0 && updateCoefs && ParallelDescriptor::IOProcessor()) {
            std::cout << "DDOp::apply: Setting coefficients at level : " << Tbd.mgLevel() << std::endl;
    }

    for (MFIter mfi(inYT); mfi.isValid(); ++mfi)
    {
        FArrayBox& outYHc = outYH[mfi];
        const FArrayBox& YTc = inYT[mfi];
        const FArrayBox& c = coefs[mfi];
        const Box& box = mfi.validbox();

        // Actually only need this if for_T0_H1 == 1
        FcpDTc.resize(box,1);
        FcpDTc.setVal(0);

        Box gbox = Box(box).grow(nGrow);
        CPic.resize(gbox,Nspec);
        chem->getCpGivenT(CPic,YTc,gbox,sCompT,0);

        // Actually only need this if for_T0_H1 == 1
        Hic.resize(gbox,Nspec);
        chem->getHGivenT(Hic,YTc,gbox,Nspec,0);

        Xc.resize(gbox,Nspec);
        chem->massFracToMoleFrac(Xc,YTc,gbox,sCompY,0);

        if (updateCoefs) {
            setCoefficients(mfi,YTc,CPic);
        }

        int fillAlpha = getAlpha;
        FArrayBox& alfc = (fillAlpha ? (*alpha)[mfi] : dum);
        
        for (int dir=0; dir<BL_SPACEDIM; ++dir) {

            const Box ebox = BoxLib::surroundingNodes(box,dir);

            // Actually only need this if for_T0_H1 == 1
            FcpDTe.resize(ebox,Nspec);
            
            // Returns fluxes (and Fn.cpn.gradT if for T)
            FArrayBox& fe = fluxYH[dir][mfi];
            const FArrayBox& ae = area[dir][mfi];
            Real thisDx = dx[dir];

            FORT_DDFLUX(box.loVect(), box.hiVect(), &thisDx, &dir,
                        fe.dataPtr(), ARLIM(fe.loVect()), ARLIM(fe.hiVect()),
                        FcpDTe.dataPtr(), ARLIM(FcpDTe.loVect()),  ARLIM(FcpDTe.hiVect()),
                        YTc.dataPtr(), ARLIM(YTc.loVect()),  ARLIM(YTc.hiVect()),
                        Xc.dataPtr(), ARLIM(Xc.loVect()),  ARLIM(Xc.hiVect()),
                        c.dataPtr(), ARLIM(c.loVect()),  ARLIM(c.hiVect()),
                        CPic.dataPtr(), ARLIM(CPic.loVect()),  ARLIM(CPic.hiVect()),
                        ae.dataPtr(), ARLIM(ae.loVect()),  ARLIM(ae.hiVect()),
                        &for_T0_H1, Hic.dataPtr(), ARLIM(Hic.loVect()), ARLIM(Hic.hiVect()),
                        &fillAlpha, alfc.dataPtr(), ARLIM(alfc.loVect()), ARLIM(alfc.hiVect()),
                        Full0_Mix1);

            // If for T, increment running sum on cell centers with -avg(F.cp.gT) across faces (in FcpDTe).
            // If for H, q was incremented with +He.Fe inside DDFLUX, nothing more to do
            if (for_T0_H1 == 0) {                
                const int diff0_avg1 = 1;
                const Real a = -1;
                const int oc = 1;
                FORT_DDETC(box.loVect(),box.hiVect(),
                           FcpDTc.dataPtr(),ARLIM(FcpDTc.loVect()),ARLIM(FcpDTc.hiVect()),
                           FcpDTe.dataPtr(),ARLIM(FcpDTe.loVect()),ARLIM(FcpDTe.hiVect()),
                           &a, &dir, &oc, &diff0_avg1);
            }
            
            // Now form -Div(F.Area) add to running total
            const int diff0_avg1 = 0;
            const Real a = -1.;
            FORT_DDETC(box.loVect(),box.hiVect(),
                       outYHc.dataPtr(),ARLIM(outYHc.loVect()),ARLIM(outYHc.hiVect()),
                       fe.dataPtr(),ARLIM(fe.loVect()),ARLIM(fe.hiVect()), &a, &dir, &nc, &diff0_avg1);            
        }
        // Form -(1/Vol) Div(F.Area)
        for (int n=0; n<nc; ++n) {
            outYHc.mult(volInv[mfi],0,n,1);
        }

        // For rho.DT/Dt, form -(1/Vol) Div(F.Area) - avg(F.cp.DT)
        if (for_T0_H1 == 0) {
            outYHc.plus(FcpDTc,0,Nspec,1);
        }

        // Build 1/cpb (CPic <- CPic.Y, CPic_0=sum(CPic_n)
        CPic.mult(YTc,0,0,Nspec);
        for (int n=1; n<Nspec; ++n) {
            CPic.plus(CPic,n,0,1);
        }
        CPic.invert(1,0,1);
        
        // For rho.DT/Dt, form (1/Cp) [ -(1/Vol) Div(F.Area) - avg(F.cp.DT) ]
        if (for_T0_H1 == 0) {
            outYHc.mult(CPic,0,Nspec,1);
        }

        if (getAlpha) {
            for (int n=0; n<nc; ++n) {
                alfc.mult(volInv[mfi],0,n,1);
            }
            alfc.mult(CPic,0,Nspec,1);

            // Modify coefficient based on what was needed to fill adjacent grow cells
            for (OrientationIter oitr; oitr; ++oitr) {
                const Orientation face = oitr();
                const FabSet& stfs = stencilWeight[face];
                int dir = face.coordDir();
                int shiftCells = ( face.isLow() ? -1 : +1 ); 

                const FArrayBox& src = stfs[mfi];
                const Box& srcBox = src.box();
                const Box dstBox = Box(srcBox).shift(dir,shiftCells); 
                    
                // Do T component
                {
                    int sComp = 1; // Stencil coef for T in slot 1
                    int dComp = Nspec;
                    alfc.mult(src,srcBox,dstBox,sComp,dComp,1);
                    alfc.plus(alfc,dstBox,srcBox,dComp,dComp,1);
                }
                // Do Y component
                {
                    int sComp = 0; // stencil coef for all Y in slot 0
                    for (int i=0; i<Nspec; ++i) {
                        int dComp = i;
                        alfc.mult(src,srcBox,dstBox,sComp,dComp,1);
                        alfc.plus(alfc,dstBox,srcBox,dComp,dComp,1);
                    }
                }
            }
        }
    }
}

void
DDOp::average (MultiFab&       mfC,
               int             dCompC,
               const MultiFab& mfF,
               int             sCompF,
               int             nComp)
{
    BL_ASSERT(mfC.nComp()>=dCompC+nComp);
    BL_ASSERT(mfF.nComp()>=sCompF+nComp);
    for (MFIter mfi(mfC); mfi.isValid(); ++mfi)
    {
        FArrayBox& C = mfC[mfi];
        const FArrayBox& F = mfF[mfi];

        const Box& cbox = mfi.validbox();
        FORT_DDCCAVG(C.dataPtr(dCompC),ARLIM(C.loVect()), ARLIM(C.hiVect()),
                     F.dataPtr(sCompF),ARLIM(F.loVect()), ARLIM(F.hiVect()),
                     cbox.loVect(), cbox.hiVect(), &nComp, MGIV.getVect());
    }
}

void
DDOp::interpolate (MultiFab&       mfF,
                   int             dCompF,
                   const MultiFab& mfC,
                   int             sCompC,
                   int             nComp)
{
    BL_ASSERT(mfF.nComp()>=dCompF+nComp);
    BL_ASSERT(mfC.nComp()>=sCompC+nComp);
    for (MFIter mfi(mfC); mfi.isValid(); ++mfi)
    {
        FArrayBox& F = mfF[mfi];
        const FArrayBox& C = mfC[mfi];
        const Box cbox = mfi.validbox();
        FORT_DDCCINT(F.dataPtr(dCompF),ARLIM(F.loVect()), ARLIM(F.hiVect()),
                     C.dataPtr(sCompC),ARLIM(C.loVect()), ARLIM(C.hiVect()),
                     cbox.loVect(), cbox.hiVect(), &nComp, MGIV.getVect());
    }
}
