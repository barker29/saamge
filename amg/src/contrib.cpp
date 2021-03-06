/*! \file

    SAAMGE: smoothed aggregation element based algebraic multigrid hierarchies
            and solvers.

    Copyright (c) 2018, Lawrence Livermore National Security,
    LLC. Developed under the auspices of the U.S. Department of Energy by
    Lawrence Livermore National Laboratory under Contract
    No. DE-AC52-07NA27344. Written by Delyan Kalchev, Andrew T. Barker,
    and Panayot S. Vassilevski. Released under LLNL-CODE-667453.

    This file is part of SAAMGE. 

    Please also read the full notice of copyright and license in the file
    LICENSE.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License (as
    published by the Free Software Foundation) version 2.1 dated February
    1999.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms and
    conditions of the GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this program; if not, see
    <http://www.gnu.org/licenses/>.
*/

#include "common.hpp"
#include "contrib.hpp"
#include <mfem.hpp>
#include "aggregates.hpp"
#include "xpacks.hpp"
#include "mbox.hpp"

namespace saamge
{
using namespace mfem;

// int abs_pair_compare(const std::pair<double, int> *a, const std::pair<double, int> *b)
int abs_pair_compare(const void *va, const void *vb)
{
    std::pair<double, int> *a, *b;
    a = (std::pair<double, int>*) va;
    b = (std::pair<double, int>*) vb;
    if (fabs(a->first) < fabs(b->first))
        return -1;
    else if (fabs(a->first) > fabs(b->first))
        return 1;
    else
        return 0;
}

ContribTent::ContribTent(int ND, bool avoid_ess_bdr_dofs_in) :
    rows(ND),
    filled_cols(0),
    avoid_ess_bdr_dofs(avoid_ess_bdr_dofs_in),
    svd_eps(1.e-10),
    threshold_(0.0)
{
    tent_interp_ = new SparseMatrix(rows);
    // next line should be optional?
    local_coarse_one_representation = new Array<double>();
}

ContribTent::~ContribTent()
{
}

SparseMatrix * ContribTent::contrib_tent_finalize()
{
    SparseMatrix *tent_interp_l;
    // on very coarse levels, the following assertion sometimes fails...
    // SA_ASSERT(tent_int_struct->filled_cols > 0);
    SA_ASSERT(filled_cols >= 0);
    if (filled_cols == 0)
        SA_PRINTF("%s","WARNING! no coarse degrees of freedom on this processor.\n");
    tent_interp_->Finalize();
    tent_interp_l = new SparseMatrix(tent_interp_->GetI(),
                                     tent_interp_->GetJ(),
                                     tent_interp_->GetData(),
                                     tent_interp_->Size(),
                                     filled_cols);
    tent_interp_->LoseData();
    delete (tent_interp_);
    SA_ASSERT(tent_interp_l->GetI() && tent_interp_l->GetJ() &&
              tent_interp_l->GetData());
    SA_ASSERT(tent_interp_l->Size() == rows);
    SA_ASSERT(tent_interp_l->Width() == filled_cols);

    return tent_interp_l;
}

/**
   Extract some of the logic, in terms of essential boundary conditions,
   from contrib_tent_insert_from_local() so we can do SVD after
   we do this
*/
void ContribTent::contrib_filter_boundary(const agg_partitioning_relations_t& agg_part_rels,
                                          DenseMatrix& local, 
                                          const int *restriction)
{
    const int vects = local.Width();
    const int dim = local.Height();
    double *data = local.Data();
    int col, i, j;

    double * newdata = new double[vects * dim];
    double * newdatap = newdata;

    SA_ASSERT(vects > 0);
    // SA_ASSERT(dim >= vects); // can we assume this will be taken care of by SVD?

    col = 0;
    double newcolumn[dim];
    for (i=0; i < vects; (++i), (data += dim))
    {
        bool atleastone = false;
        int numremoved = 0;
        for (j=0; j < dim; ++j)
        {
            SA_ASSERT(data + j < local.Data() + dim*vects);
            const int row = restriction[j];
            const double a = data[j];

            if (a == 0.0 ||
                (avoid_ess_bdr_dofs &&
                 agg_is_dof_on_essential_border(agg_part_rels, row)))
            {
                if (SA_IS_OUTPUT_LEVEL(7) && 0. != a)
                    SA_ALERT_PRINTF("Non-zero DoF on essential boundary."
                                    " Ignoring entry: %g!", a); // just a single entry here...
                newcolumn[j] = 0.0;
                numremoved++;
                continue;
            }
            atleastone = true;
            newcolumn[j] = a;
        }

        if (atleastone)
        {
            for (j=0; j<dim; ++j)
            {
                newdatap[j] = newcolumn[j];
            }
            newdatap += dim;
            col++;
        }
        else
        {
            SA_ALERT_PRINTF("%s","Entire column is zero, possibly because of essential boundary, ignoring column!");
        }
    }
    local.SetSize(dim, col);
    data = local.Data();
    for (int j=0; j<dim * local.Width(); ++j)
        data[j] = newdata[j];
    delete [] newdata;
}

/**
   Separated from contrib_tent_insert_from_local(),
   separating the essential boundary checking from the actual
   insertion so we can do SVD in between
*/
void ContribTent::contrib_tent_insert_simple(
    const agg_partitioning_relations_t& agg_part_rels, DenseMatrix& local, 
    const int *restriction)
{
    const int vects = local.Width();
    const int dim = local.Height();
    double *data = local.Data();
    int col, i, j;

    SA_ASSERT(vects > 0);
    SA_ASSERT(dim >= vects);

    col = filled_cols;
    for (i=0; i < vects; (++i), (data += dim))
    {
        for (j=0; j< dim; ++j)
        {
            const int row = restriction[j];
            if (fabs(data[j]) > threshold_)
                tent_interp_->Set(row, col, data[j]);
        }
        ++col;
    }
    filled_cols = col;
}


/**
   This routine changed ATB 11 May 2015 to also modify local according
   to boundary conditions, not just the global interp, because I think
   this is mathematically the right thing to do and because we now store
   local as a representative of coarse DoFs for multilevel extension

   Recently in multilevel setting we are moving from this to a combination 
   of contrib_filter_boundary() and contrib_tent_insert_simple() .

   DEPRECATED
*/
void ContribTent::contrib_tent_insert_from_local(
    const agg_partitioning_relations_t& agg_part_rels,
    DenseMatrix& local, const int *restriction)
{
    const int vects = local.Width();
    const int dim = local.Height();
    double *data = local.Data();
    bool modified = false; // set to true if newdata != data
    int col, i, j;

    double * newdata = new double[vects * dim];
    double * newdatap = newdata;

    SA_ASSERT(vects > 0);
    SA_ASSERT(dim >= vects);

    int firstcol = filled_cols;
    col = filled_cols;
    double newcolumn[dim];
    int bestcase_nonzerodofs = 0;
    int nonzerodofs = 0;
    for (i=0; i < vects; (++i), (data += dim))
    {
        nonzerodofs = 0;
        double adhoc_column_norm = 0.0;
        for (j=0; j < dim; ++j)
        {
            SA_ASSERT(data + j < local.Data() + dim*vects);
            const int row = restriction[j];
            const double a = data[j];

            if (a == 0.0 ||
                (avoid_ess_bdr_dofs &&
                 agg_is_dof_on_essential_border(agg_part_rels, row)))
            {
                if (SA_IS_OUTPUT_LEVEL(7) && 0. != a)
                    SA_ALERT_PRINTF("Non-zero DoF on essential boundary."
                                    " Ignoring entry: %g!", a); // just a single entry here...
                newdatap[j] = 0.0; // new ATB 11 May 2015
                newcolumn[j] = 0.0;
                modified = true;
                continue;
            }
            nonzerodofs++;
            adhoc_column_norm += fabs(a);
            newcolumn[j] = a;
        }
        if (nonzerodofs > bestcase_nonzerodofs)
            bestcase_nonzerodofs = nonzerodofs;

        // not clear what the right tolerance here is, especially with varying coefficients
        if (adhoc_column_norm < 1.e-3) 
        {
            SA_ALERT_PRINTF("Tentative prolongator column is near zero, "
                            "l1 norm %e, Ignoring column!",
                            adhoc_column_norm);
            modified = true;
        }
        else 
        {
            if (adhoc_column_norm < 1.e-1)
                SA_ALERT_PRINTF("Accepting column of small l1 norm %e!",
                                adhoc_column_norm);
            for (j=0; j< dim; ++j)
            {
                const int row = restriction[j];
                tent_interp_->Set(row, col, newcolumn[j]);
                newdatap[j] = newcolumn[j];
            }
            ++col;
            newdatap += dim;
        }
    }
    SA_ASSERT(bestcase_nonzerodofs >= col - firstcol);
    if (modified)
    {
        local.SetSize(dim, col - filled_cols);
        data = local.Data();
        for (int j=0; j<dim * local.Width(); ++j)
            data[j] = newdata[j];
    }   
    delete [] newdata;
    filled_cols = col;
}

void ContribTent::ExtendWithConstants(
    DenseMatrix ** received_mats,
    const agg_partitioning_relations_t& agg_part_rels)
{
    Vector coords;
    ExtendWithPolynomials(received_mats, agg_part_rels, 0, -1, -1, coords);
}

/// TODO: this is probably unsensible with elasticity
void ContribTent::ExtendWithPolynomials(
    DenseMatrix ** received_mats,
    const agg_partitioning_relations_t& agg_part_rels,
    int order, int spatial_dimension, 
    int num_nodes, const Vector& coords)
{
    int num_mises = agg_part_rels.num_mises;
    for (int mis=0; mis<num_mises; ++mis)
    {
        int owner = agg_part_rels.mis_master[mis];
        if (owner == PROC_RANK)
        {
            const int mis_size = agg_part_rels.mises_size[mis];
            // the next bit is a real hack...
            if (received_mats[mis] == NULL)
            {
                received_mats[mis] = new DenseMatrix[1];
                received_mats[mis][0].SetSize(mis_size,0);
            }
            DenseMatrix& local_spectral = received_mats[mis][0];
            SA_ASSERT(local_spectral.Height() == mis_size);
            const int swidth = local_spectral.Width();
            int newwidth;
            if (order == 0)
                newwidth = swidth + 1;
            else if (order == 1)
                newwidth = swidth + spatial_dimension + 1;
            else
                throw 1;   
            DenseMatrix extended(local_spectral.Height(), newwidth);
            const int * row = agg_part_rels.mis_to_dof->GetRow(mis);
            for (int k=0; k<mis_size; ++k)
            {
                for (int j=0; j<local_spectral.Width(); ++j)
                    extended.Elem(k,j) = local_spectral.Elem(k,j);
                extended.Elem(k,swidth) = 1.0;
                if (order == 1)
                {
                    for (int d=0; d<spatial_dimension; ++d)
                    {
                        int dof_num = row[k];
                        extended.Elem(k,swidth+d+1) = 
                            coords(num_nodes*d + dof_num);
                    }
                }
            }
            local_spectral = extended;
        }
    }
}

void ContribTent::ExtendWithRBMs(
    DenseMatrix ** received_mats,
    const agg_partitioning_relations_t& agg_part_rels,
    int spatial_dimension,
    int num_nodes, const Vector& coords)
{
    // follow ExtendWithPolynomials pretty closely
    // three RBMs in 2D, six in 3D (see eg. Hughes p. 88)
    // SVD is done later
    // We are assuming the FES is Ordering::ByVDIM here (should assert)

    int num_mises = agg_part_rels.num_mises;
    for (int mis=0; mis<num_mises; ++mis)
    {
        int owner = agg_part_rels.mis_master[mis];
        if (owner == PROC_RANK)
        {
            // contents of MIS are SAAMGe dofs, ie, there are (dimension)*(num_nodes) of these dofs
            const int mis_size = agg_part_rels.mises_size[mis];
            // the next bit is a real hack...
            if (received_mats[mis] == NULL)
            {
                received_mats[mis] = new DenseMatrix[1];
                received_mats[mis][0].SetSize(mis_size,0);
            }
            DenseMatrix& local_spectral = received_mats[mis][0];
            SA_ASSERT(local_spectral.Height() == mis_size);
            const int swidth = local_spectral.Width();
            int newwidth;
            if (spatial_dimension == 1)
                newwidth = swidth + 1;
            else if (spatial_dimension == 2)
                newwidth = swidth + 3;
            else if (spatial_dimension == 3)
                newwidth = swidth + 6;
            else
                throw 1;
            DenseMatrix extended(local_spectral.Height(), newwidth);
            for (int k=0; k<mis_size; ++k)
            {
                for (int j=0; j<local_spectral.Width(); ++j)
                    extended.Elem(k,j) = local_spectral.Elem(k,j);
            }
            const int * row = agg_part_rels.mis_to_dof->GetRow(mis);
            SA_ASSERT(mis_size % spatial_dimension == 0);
            const int nodes_in_mis = mis_size / spatial_dimension;
            for (int node=0; node<nodes_in_mis; ++node)
            {
                int node_num = row[node*spatial_dimension] / spatial_dimension;
                SA_ASSERT(node_num < num_nodes);
                for (int d=0; d<spatial_dimension; ++d)
                {
                    const int k = node*spatial_dimension + d;
                    extended.Elem(k, swidth + d) = 1.0; // constant modes, in each of x,y,z directions
                }
                if (spatial_dimension > 1)
                {
                    const double xcoord = coords(num_nodes*0 + node_num);
                    const double ycoord = coords(num_nodes*1 + node_num);
                    int k = node*spatial_dimension + 0;
                    extended.Elem(k, swidth + spatial_dimension + 0) = ycoord;
                    k = node*spatial_dimension + 1;
                    extended.Elem(k, swidth + spatial_dimension + 0) = -xcoord;
                }
                if (spatial_dimension > 2)
                {
                    const double xcoord = coords(num_nodes*0 + node_num);
                    const double ycoord = coords(num_nodes*1 + node_num);
                    const double zcoord = coords(num_nodes*2 + node_num);
                    int k = node*spatial_dimension + 0;
                    extended.Elem(k, swidth + spatial_dimension + 1) = 0.0;
                    extended.Elem(k, swidth + spatial_dimension + 2) = -zcoord;
                    k = node*spatial_dimension + 1;
                    extended.Elem(k, swidth + spatial_dimension + 1) = zcoord;
                    extended.Elem(k, swidth + spatial_dimension + 2) = 0.0;
                    k = node*spatial_dimension + 2;
                    extended.Elem(k, swidth + spatial_dimension + 1) = -ycoord;
                    extended.Elem(k, swidth + spatial_dimension + 2) = xcoord;
                }
            }
            local_spectral = extended;
        }
    }    
}

/**
   Want to add linear functions also to coarse space.
   This requires knowing coordinates of dofs.

   Turns out we need to also add constants.
   
   coords is length (dof * spatial_dimension), is like what comes from Mesh.GetVertices()
   or Mesh.GetNodes()
*/
void ContribTent::contrib_linears(
    const agg_partitioning_relations_t& agg_part_rels,
    int spatial_dimension, int num_nodes, const Vector& coords)
{
    SA_ASSERT(coords.Size() == spatial_dimension*num_nodes);
    int num_mises = agg_part_rels.num_mises;

    // DenseMatrix ** received_mats = CommunicateEigenvectors(agg_part_rels, cut_evects_arr, sec);
    DenseMatrix ** received_mats = new DenseMatrix*[num_mises];
    std::memset(received_mats,0,sizeof(DenseMatrix*) * num_mises);

    ExtendWithPolynomials(received_mats, agg_part_rels, 1, spatial_dimension, 
                          num_nodes, coords);

    // do SVDs on owned MISes, build tentative interpolator
    int * row_sizes = new int[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
        row_sizes[mis] = 1;
    SVDInsert(agg_part_rels, received_mats, row_sizes, false);
    delete [] row_sizes;
}

/**
   uber-simplifed version of contrib_mises, under the assumption
   that we do one coarse DOF per MIS, in particular the (normalized)
   vector of all ones
*/
void ContribTent::contrib_ones(const agg_partitioning_relations_t& agg_part_rels)
{
    int num_mises = agg_part_rels.num_mises;

    // DenseMatrix ** received_mats = CommunicateEigenvectors(agg_part_rels, cut_evects_arr, sec);
    DenseMatrix ** received_mats = new DenseMatrix*[num_mises];
    std::memset(received_mats,0,sizeof(DenseMatrix*) * num_mises);

    ExtendWithConstants(received_mats, agg_part_rels);

    // do SVDs on owned MISes, build tentative interpolator
    int * row_sizes = new int[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
        row_sizes[mis] = 1;
    SVDInsert(agg_part_rels, received_mats, row_sizes, false);
    delete [] row_sizes;
}

DenseMatrix ** ContribTent::CommunicateEigenvectors(
    const agg_partitioning_relations_t& agg_part_rels,
    DenseMatrix * const *cut_evects_arr,
    SharedEntityCommunication<DenseMatrix>& sec)
{
    // restrict eigenvectors to MISes
    int num_mises = agg_part_rels.num_mises;
    DenseMatrix ** restricted_evects_array;
    restricted_evects_array = new DenseMatrix*[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
    {
        int local_AEs_containing = agg_part_rels.mis_to_AE->RowSize(mis);
        int mis_size = agg_part_rels.mises_size[mis];
        restricted_evects_array[mis] = new DenseMatrix[local_AEs_containing];
        // restrict local AE to this MIS (copied from Delyan Kalchev's contrib_ref_aggs())
        for (int ae=0; ae<local_AEs_containing; ++ae)
        {
            // SA_PRINTF("ae = %d, restriction = %p\n", ae, restriction);
            int AE_id = agg_part_rels.mis_to_AE->GetRow(mis)[ae];
            agg_restrict_to_agg_enforce(AE_id, agg_part_rels, mis_size,
                                        agg_part_rels.mis_to_dof->GetRow(mis),
                                        *(cut_evects_arr[AE_id]),
                                        restricted_evects_array[mis][ae]);
        }
    }

    // communication: collect MIS-restricted eigenvectors on the process that owns the MIS
    sec.ReducePrepare();
    for (int mis=0; mis<num_mises; ++mis)
    {
        // combine all the AEs into one DenseMatrix (this is complicated 
        // and expensive in memory but might save us latency costs...)
        int mis_size = agg_part_rels.mises_size[mis];
        int rowsize = agg_part_rels.mis_to_AE->RowSize(mis);
        int * row = agg_part_rels.mis_to_AE->GetRow(mis);
        int numvecs = 0;
        for (int j=0; j<rowsize; ++j)
        {
            int AE = row[j];
            numvecs += cut_evects_arr[AE]->Width();
        }    
        DenseMatrix send_mat(mis_size, numvecs);
        numvecs = 0;
        for (int j=0; j<rowsize; ++j)
        {
            int AE = row[j];
            std::memcpy(send_mat.Data() + numvecs*mis_size, 
                        restricted_evects_array[mis][j].Data(), 
                        mis_size * cut_evects_arr[AE]->Width() * sizeof(double));
            numvecs += cut_evects_arr[AE]->Width();
        }
        delete [] restricted_evects_array[mis];

        sec.ReduceSend(mis,send_mat);
    }
    delete [] restricted_evects_array;
    return sec.Collect();
}

void ContribTent::SVDInsert(const agg_partitioning_relations_t& agg_part_rels,
                            DenseMatrix ** received_mats, int * row_sizes,
                            bool scaling_P)
{
    int num_mises = agg_part_rels.num_mises;
    DenseMatrix lsvects;
    // TODO: can we make mis_tent_interps a pointer to array of DenseMatrix, not DenseMatrix* ?
    // maybe use a std::vector of mfem::Array or something?
    mis_tent_interps = new DenseMatrix*[num_mises]; 
    std::memset(mis_tent_interps,0,sizeof(DenseMatrix*) * num_mises);
    Vector svals;
    int num_coarse_dofs = 0;
    mis_numcoarsedof = new int[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
    {
        int owner = agg_part_rels.mis_master[mis];
        if (owner == PROC_RANK)
        {
            // int row_size = sec.NumNeighbors(mis);
            int row_size = row_sizes[mis];
            mis_tent_interps[mis] = new DenseMatrix;

            // check to see if all of this MISes DOFs are on essential boundary - copied from contrib_big_aggs()
            // this only checks the dofs for one AE, but that should be sufficient
            const int mis_size = agg_part_rels.mises_size[mis];
            const int dim = received_mats[mis][0].Height();
            SA_ASSERT(mis_size == dim);
            if (avoid_ess_bdr_dofs)
            {
                bool interior_dofs = false;
                for (int j=0; j < dim; ++j)
                {
                    const int row = agg_part_rels.mis_to_dof->GetRow(mis)[j];
                    SA_ASSERT(rows > row);
                    if (!agg_is_dof_on_essential_border(agg_part_rels, row))
                    {
                        interior_dofs = true;
                        break;
                    }
                }
                if (!interior_dofs)
                {
                    if (SA_IS_OUTPUT_LEVEL(6))
                        SA_ALERT_PRINTF("All DoFs are on essential boundary."
                                        " Ignoring the entire contribution"
                                        " introducing not more than %d vector(s)"
                                        " on an aggregate of size %d!",
                                        received_mats[mis][0].Width(), dim);
                    mis_numcoarsedof[mis] = 0;
                    // next line makes future assertions and communications cleaner, but is mostly unnecessary
                    mis_tent_interps[mis]->SetSize(dim, 0); 
                    delete [] received_mats[mis];
                    continue; // TODO: remove this, refactor
                }
            }

            if (dim == 1) // could think about a kind of identity matrix whenever dim < total width, but I think SVD will take care of this
            {
                // see assertion in contrib_tent_insert_from_local: SA_ASSERT(dim > 1 || 1. == a);
                mis_tent_interps[mis]->SetSize(1,1);
                mis_tent_interps[mis]->Elem(0,0) = 1.0;
            }
            else
            {
                int total_num_columns = 0;
                for (int q=0; q<row_size; ++q)
                {
                    contrib_filter_boundary(agg_part_rels,
                                            received_mats[mis][q],
                                            agg_part_rels.mis_to_dof->GetRow(mis));
                    total_num_columns += received_mats[mis][q].Width();
                }

                if (total_num_columns == 0)
                    svals.SetSize(0);
                else
                    xpack_svd_dense_arr(received_mats[mis], row_size, lsvects, svals);
                if (svals.Size() == 0) // we trim (near) zeros out of svals, this means all svals == 0
                {
                    SA_PRINTF("WARNING: completely zero contribution on mis %d!\n", mis);
                    SA_PRINTF("WARNING: dim = %d, row_size = %d\n", dim, row_size);
                    mis_numcoarsedof[mis] = 0;
                    mis_tent_interps[mis]->SetSize(dim, 0); // this makes future assertions and communications cleaner, but is mostly unnecessary
                    delete [] received_mats[mis];
                    continue; // TODO: remove this, refactor 
                }
                xpack_orth_set(lsvects, svals, *mis_tent_interps[mis], svd_eps);
            }
            if (agg_part_rels.testmesh)
            {
                std::stringstream filename;
                filename << "mis_tent_interp_" << mis << "." << PROC_RANK << ".densemat";
                std::ofstream out(filename.str().c_str());
                mis_tent_interps[mis]->Print(out);
            }
            int filled_cols_l = filled_cols;

            contrib_tent_insert_simple(agg_part_rels,
                                       *mis_tent_interps[mis], 
                                       agg_part_rels.mis_to_dof->GetRow(mis));

            filled_cols_l = filled_cols - filled_cols_l;

            SA_ASSERT(filled_cols_l == mis_tent_interps[mis]->Width());
            if (scaling_P && filled_cols_l > 0) 
            {
                Vector x(mis_tent_interps[mis]->Width());  // size of coarse dofs for this MIS
                Vector b(mis_tent_interps[mis]->Height());
                b = 1.0;
                xpack_solve_lls(*mis_tent_interps[mis],b,x);
                double norm = 0.0;
                for (int k=0; k<x.Size(); ++k)
                    norm += x(k)*x(k);
                norm = std::sqrt(norm);
                // we can append because the coarse DOF are numbered in exactly this order, by MIS
                for (int k=0; k<x.Size(); ++k)
                    local_coarse_one_representation->Append(x(k) / norm);
            }
            mis_numcoarsedof[mis] = filled_cols_l;
            num_coarse_dofs += filled_cols_l;
            delete [] received_mats[mis];
        }
        else
        {
            mis_numcoarsedof[mis] = 0; 
            // we only do this new so deletion is cleaner at the end, could avoid it at the cost of more ifs
            mis_tent_interps[mis] = new DenseMatrix;
            // tent_int_struct->mis_tent_interps[mis]->SetSize(0,0); // ???
        }
    }
    delete [] received_mats;

    coarse_truedof_offset = 0;
    MPI_Scan(&num_coarse_dofs,&coarse_truedof_offset,1,MPI_INT,MPI_SUM,PROC_COMM);
    coarse_truedof_offset -= num_coarse_dofs;
    SA_RPRINTF_L(PROC_NUM-1, 8, "coarse_truedof_offset = %d\n",coarse_truedof_offset);
}

/**
   Takes solutions to spectral problems on AEs, restricts to MISes, does
   appropriate communication and SVD, and constructs tentative prolongator

   This is one of the key communication routine for the multilevel MIS extension
   to this solver

   Possibly more attention needs to be paid to boundary conditions and
   small (1-2 dof) MISes
*/
void ContribTent::contrib_mises(
    const agg_partitioning_relations_t& agg_part_rels,
    DenseMatrix * const *cut_evects_arr, bool scaling_P)
{
    SharedEntityCommunication<DenseMatrix> sec(PROC_COMM,
                                               *agg_part_rels.mis_truemis);
    DenseMatrix ** received_mats = CommunicateEigenvectors(agg_part_rels, cut_evects_arr, sec);

    // do SVDs on owned MISes, build tentative interpolator
    int num_mises = agg_part_rels.num_mises;
    int * row_sizes = new int[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
        row_sizes[mis] = sec.NumNeighbors(mis);
    SVDInsert(agg_part_rels, received_mats, row_sizes, scaling_P);
    delete [] row_sizes;
}

void ContribTent::contrib_composite(
    const agg_partitioning_relations_t& agg_part_rels,
    DenseMatrix * const *cut_evects_arr, int polynomial_order,
    int spatial_dimension, int num_nodes, const Vector& coords)
{
    const bool scaling_P = false;

    SharedEntityCommunication<DenseMatrix> sec(PROC_COMM,
                                               *agg_part_rels.mis_truemis);
    DenseMatrix ** received_mats = 
        CommunicateEigenvectors(agg_part_rels, cut_evects_arr, sec);

    if (num_nodes == agg_part_rels.ND)
    {
        ExtendWithPolynomials(received_mats, agg_part_rels, polynomial_order,
                              spatial_dimension, num_nodes, coords);
    }
    else
    {
        ExtendWithRBMs(received_mats, agg_part_rels, spatial_dimension,
                       num_nodes, coords);
    }

    // do SVDs on owned MISes, build tentative interpolator
    int num_mises = agg_part_rels.num_mises;
    int * row_sizes = new int[num_mises];
    for (int mis=0; mis<num_mises; ++mis)
        row_sizes[mis] = sec.NumNeighbors(mis);
    SVDInsert(agg_part_rels, received_mats, row_sizes, scaling_P);
    delete [] row_sizes;
}

} // namespace saamge
