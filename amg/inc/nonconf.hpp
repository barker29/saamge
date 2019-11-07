/*
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

/**
    @file This file and the respective .cpp file implement a nonconforming interior penalty (IP) AMGe
    approach. It basically "breaks" the space with the first coarsening and can reduce the problem to
    faces similar to static condensation. It is thus natural and easy to recursively extend to multiple levels and
    is potentially suitable for high-order discretizations when a matrix-free approach is utilized.

    The main difference is in the first (finest) coarsening. On that level, the agglomerated (coarse) faces need to be obtained.
    Coarse faces are essentially certain MISes in terms of faces. Once the space is "broken" (and, thus, nonconforming in the sense
    that the coarse spaces are not subspaces of the finest), it remains "broken" on all coarse levels (which are nested). Therefore,
    after the first coarsening, or after obtaining the IP formulation, the rest of the coarsening procedures are very similar to
    the usual SAAMGe and now faces can be coarsened by simply considering MISes in terms of DoFs, since the faces were already
    separated (there are no corner DoFs that are shared between faces) and are entirely characterized by the DoFs on them.

    It is more convenient to keep this method separate from the rest of SAAMGe as it works in a slightly different way.
    That is why this is considered as a semi-separate module.

    Currently, this is aimed at solver hierarchies, although it may have some potential to become an upscaling approach for
    coarse discretizations.

    TODO: Matrix-free implementation. It may be sufficient to consider the idea of matrix-free implementations only on the
          finest level (first coarsening), since this is the most meaningful and it is less clear if there will be any benefit
          to have the coarse levels matrix-free.

    TODO: Consider making this easy to use for hybridized systems.
*/

#pragma once
#ifndef _NONCONF_HPP
#define _NONCONF_HPP

#include "common.hpp"
#include <mfem.hpp>
#include "tg_data.hpp"
#include "interp.hpp"
#include "aggregates.hpp"
#include "elmat.hpp"

namespace saamge
{

/**
   @brief Solves via Schur complement.

   Reduces the problem (by elimination) to a Schur complement system, then uses the given \a solver for that
   system and, in the end, recovers the eliminated (by backward substitution) variables.

   That is, reduces the problem to the cface space, solves or preconditions the reduced system, and
   recovers to the full space, including "interiors".
*/
class SchurSolver : public mfem::Solver
{
public:
    SchurSolver(const interp_data_t& interp_data, const agg_partitioning_relations_t& agg_part_rels,
                const mfem::HypreParMatrix& cface_cDof_TruecDof, const mfem::HypreParMatrix& cface_TruecDof_cDof, const mfem::Solver& solver, bool rand_init_guess=false)
        : interp_data(interp_data), agg_part_rels(agg_part_rels), cface_cDof_TruecDof(cface_cDof_TruecDof),
          cface_TruecDof_cDof(cface_TruecDof_cDof), solver(solver), rand_init_guess(rand_init_guess) {}
    virtual void SetOperator(const mfem::Operator &op) {}
    virtual void Mult(const mfem::Vector &x, mfem::Vector &y) const;
private:
    const interp_data_t& interp_data;
    const agg_partitioning_relations_t& agg_part_rels;
    const mfem::HypreParMatrix& cface_cDof_TruecDof;
    const mfem::HypreParMatrix& cface_TruecDof_cDof;
    const mfem::Solver& solver;
    const bool rand_init_guess;
};

/**
    Assembles the global rhs coming from eliminating the "interior" DoFs. The output vector
    is represented in terms of true cface DoFs (i.e., defined only on cface DoFs)
    and must be freed by the caller. The input vector is in terms of true DoFs that also
    include the "interior" DoFs. This is not much of a challenge, since all "interior" dofs are
    always true dofs (no sharing) and adding and removing interior dofs is essentially working with
    \a interp_data.celements_cdofs number of dofs at the beginning of the vector. The rest of the vector
    is filled with cface dofs only.

    \a cface_TruecDof_cDof is in terms of the cface dofs only (i.e., excluding the
    "interior" dofs).
*/
mfem::HypreParVector *nonconf_assemble_schur_rhs(const interp_data_t& interp_data,
                    const agg_partitioning_relations_t& agg_part_rels,
                    const mfem::HypreParMatrix& cface_TruecDof_cDof, const mfem::Vector& rhs);

/**
    Performs the backward substitution from the block elimination. It takes the full
    (including "interiors") original rhs in true DoFs and the face portion of the
    (obtained by inverting the Schur complement) solution in face true DoFs (excluding "interiors").
    The returned vector (i.e., x) is in terms of all (including "interiors") true DoFs.
    This is not much of a challenge, since all "interior" dofs are
    always true dofs (no sharing) and adding and removing interior dofs is essentially working with
    \a interp_data.celements_cdofs number of dofs at the beginning of the vector. The rest of the vector
    is filled with cface dofs only.

    \a cface_cDof_TruecDof is in terms of the cface dofs only (i.e., excluding the
    "interior" dofs).
*/
void nonconf_schur_update_interior(const interp_data_t& interp_data,
                    const agg_partitioning_relations_t& agg_part_rels, const mfem::HypreParMatrix& cface_cDof_TruecDof,
                    const mfem::Vector& rhs, const mfem::Vector& facev, mfem::Vector& x);

/**
    Once all dense element Schur complement matrices (on cfaces) are obtained,
    the global Schur complement matrix is assembled by a standard procedure in this
    routine. The procedure is standard but adapted for the particular data structures
    and organization. Can be used for both fine and coarse scale discretizations.

    The returned global Schur complement matrix must be freed by the caller.
*/
mfem::HypreParMatrix *nonconf_assemble_schur_matrix(const interp_data_t& interp_data,
                    const agg_partitioning_relations_t& agg_part_rels,
                    const mfem::HypreParMatrix& cface_cDof_TruecDof);

/*! A Schur complement smoother matching the bulky SAAMGe C-type abstraction.

    Just like a Schur complement solver but instead of inverting the Schur complement,
    only smoothing is performed. In more detail, it computes the residual, eliminates
    what needs to be eliminated, smooths, using the Schur complement, then substitutes back
    to the full size of the vector and updates /a x.

    The smoother and Schur complement data is in \a data, which is of type \b smpr_poly_data_t.

    XXX: This needed to add some stuff to \b smpr_poly_data_t to carry all the information,
         which I kind of dislike.
*/
void nonconf_schur_smoother(mfem::HypreParMatrix& A, const mfem::Vector& b, mfem::Vector& x, void *data);

/*! Builds a "coarse" interior penalty formulation and the respective space. \a tg_data should already
    have some basic initializations via tg_init_data().

    In fact it may or may not coarsen at all. If it coarsens, it computes eigenvectors for the
    local H1 matrix and obtains basis functions. If not coarsening, it just obtains identity basis.
    In all cases, the essential boundary dofs are eliminated, in the sense that all basis functions
    vanish on the essential portion of the boundary.

    It obtains the "coarse" IP matrix, or a Schur complement on the faces, and the transition operator
    from the H1 space to the IP spaces (straight to the coarse ones if actual coarsening is employed).

    It also fills-in, in \a agg_part_rels, the "dof to true dof" (coarse or fine) relations for the IP
    spaces. It does it appropriately, respecting what dofs actually remain, whether Schur complement is
    employed or not.

    \a full_space is a debug feature. It is used to generate a fine-scale IP discretization. I.e.,
    no actual coarsening is performed.

    \a diagonal gives the option to provide a vector that serves as a diagonal matrix (of size equal to
    the number of dofs on the processor), whose restrictions define inner products for the agglomerate face
    penalty terms. It is generic but the main intent is to be used to provide the entries of the global stiffness
    matrix diagonal. In parallel, some communication would be needed on the shared dofs so that all entries
    of interest become known to the processor, i.e., the processor needs to know the entries for all dofs it sees
    NOT just the dofs it owns. This involves a dof_truedof application.

    XXX: It uses the agglomerate H1 matrix for the eigenvalue problem and distributes the eigenvectors
         to obtain the final basis (after SVD). That is, even though the "coarse" space is for the
         IP formulation, the eigenvalue problems do not account for the specifics of a respective
         fine-scale IP formulation.
    XXX: It is tuned towards coarse spaces, so it utilizes dense matrices, which can be slow when
         \a full_space is on.
*/
void nonconf_ip_coarsen_finest_h1(tg_data_t& tg_data, agg_partitioning_relations_t& agg_part_rels,
                                  ElementMatrixProvider *elem_data, double theta, double delta,
                                  const mfem::Vector *diagonal=NULL, bool schur=true, bool full_space=false);

/*! Builds the right-hand side for the "fine" interior penalty formulation.
    The returned vector must be freed by the caller.
*/
mfem::HypreParVector *nonconf_ip_discretization_rhs(const tg_data_t& tg_data,
                                              const agg_partitioning_relations_t& agg_part_rels,
                                              ElementMatrixProvider *elem_data);

/*! Prepare "identity" element basis and "interior" stiffness matrix, removing all essential BCs' DoFs.

    It is concerned with a fine-scale IP setting. Concentrates on the "interiror" portions of the agglomerates,
    which essentially correspond to the H1 agglomerates. All it does is remove the essential boundary dofs, by deleting the rows
    and columns in the H1 agglomerate matrices and creating a fine-scale "interior" basis that skips those dofs, i.e.,
    the basis functions vanish on the essential portion of the boundary.

    If \a orig_AEs_stiffm is provided, it will be used to return the original H1 agglomerate matrices with boundary entries.
    Then, the returned matrices must be freed by the caller.
*/
void nonconf_eliminate_boundary_full_element_basis(interp_data_t& interp_data, const agg_partitioning_relations_t& agg_part_rels,
                                                   ElementMatrixProvider *elem_data, mfem::Matrix **orig_AEs_stiffm=NULL);

/*! Builds a "fine-scale" interior penalty formulation and the respective spaces using (or abusing) the TG structure.
    Essential BCs are removed from the space via having vanishing basis functions on that portion of the boundary.
    \a tg_data should already have some basic initializations via tg_init_data().

    It can also use straight the Schur complement on the agglomerate faces. Note that the faces are agglomerate,
    but the dofs are fine-scale.

    It also fills-in, in \a agg_part_rels, the "dof to true dof" relations for the IP
    spaces. It does it appropriately, respecting what dofs actually remain, whether Schur complement is
    employed or not.

    \a diagonal gives the option to provide a vector that serves as a diagonal matrix (of size equal to
    the number of dofs on the processor), whose restrictions define inner products for the agglomerate face
    penalty terms. It is generic but the main intent is to be used to provide the entries of the global stiffness
    matrix diagonal. In parallel, some communication would be needed on the shared dofs so that all entries
    of interest become known to the processor, i.e., the processor needs to know the entries for all dofs it sees
    NOT just the dofs it owns. This involves a dof_truedof application.

    XXX: The approach here is more direct than \b nonconf_ip_coarsen_finest_h1 with the \b full_space option.
         That is why it allows using sparse matrices.
*/
void nonconf_ip_discretization(tg_data_t& tg_data, agg_partitioning_relations_t& agg_part_rels,
                               ElementMatrixProvider *elem_data, double delta,
                               const mfem::Vector *diagonal=NULL, bool schur=false);

/*! Builds a "coarse" interior penalty formulation and the respective space. \a tg_data should already
    have some basic initializations via tg_init_data().

    It computes eigenvectors for the local fine IP matrix and obtains basis functions.
    The essential boundary dofs are eliminated, in the sense that all basis functions
    vanish on the essential portion of the boundary.

    It obtains the "coarse" IP matrix, or a Schur complement on the faces, and the transition operator
    from the H1 space to the IP spaces (straight to the coarse ones).

    It also fills-in, in \a agg_part_rels, the "dof to true dof" (coarse or fine) relations for the IP
    spaces. It does it appropriately, respecting what dofs actually remain, whether Schur complement is
    employed or not.

    \a diagonal gives the option to provide a vector that serves as a diagonal matrix (of size equal to
    the number of dofs on the processor), whose restrictions define inner products for the agglomerate face
    penalty terms. It is generic but the main intent is to be used to provide the entries of the global stiffness
    matrix diagonal. In parallel, some communication would be needed on the shared dofs so that all entries
    of interest become known to the processor, i.e., the processor needs to know the entries for all dofs it sees
    NOT just the dofs it owns. This involves a dof_truedof application.

    XXX: It uses the agglomerate IP matrix for the eigenvalue problem and distributes the eigenvectors
         to obtain the final basis (after SVD).
    XXX: It is tuned towards coarse spaces, so it utilizes dense matrices.
*/
void nonconf_ip_coarsen_finest_ip(tg_data_t& tg_data, agg_partitioning_relations_t& agg_part_rels,
                                  ElementMatrixProvider *elem_data, double theta, double delta,
                                  const mfem::Vector *diagonal=NULL, bool schur=false);

/*! Generates \b AE_to_dof for the IP formulation including dofs associated with the
    agglomerates and with the agglomerate faces. Recall that the nonconforming spaces are defined on the
    level of agglomerates. Works for coarse and fine spaces and, also,
    for condensed (Schur complement) or not formulations.

    The returned table must be freed by the caller.
*/
mfem::Table *
nonconf_create_AE_to_dof(const agg_partitioning_relations_t& agg_part_rels_nonconf,
                         const interp_data_t& interp_data_nonconf);

/*! Generates partitioning relations to be used by SAAMGe to solve the interior penalty problem.

    Here, \a agg_part_rels_nonconf and \a interp_data_nonconf are generated by one of the routines
    in this module, that produce the non-conforming spaces and formulations. Using the input, a partitioning structure
    is generated that is usable in SAAMGe and is formulated in terms of the IP entities. Namely, elements and
    agglomerates remain unchanged but the dofs are different. Note that the non-conforming spaces are defined on the
    level of agglomerates, so the main part is the generation of \b AE_to_dof, while elem_to_dof
    makes no sense in general, so it is NOT produced. In the end, it generates MISes.

    It works for both the entire IP space or the one for the Schur complement (the condensed IP formulation),
    which includes only the agglomerate face spaces. The only difference is, whether "interior" dofs are
    included or not. In this context, MISes reidentify the "interiors" and agglomerate faces (or just
    the agglomerate faces if the condensed formulation is considered) in a form suitable for SAAMGe, i.e.,
    it identifies agglomerate faces in terms of dofs, rather than in terms of fine-scale faces, which at this
    stage is mathematically equivalent, since the agglomerate faces are separated from each other
    (and from the "interiors") in terms of dofs.

    XXX: It is intended to be used with a fine-scale IP formulation. In principle, it can be called on a coarse-scale
         one but it makes no much sense. The idea is to use SAAMGe to coarsen for the solver. Starting with
         a coarse formulation needs a modified interpretation of the entities. E.g., agglomerates should be
         considered as ("coarse") elements on the coarse IP space but this is not the case here. Then, one can simply use
         \b nonconf_create_AE_to_dof and name the obtained table celem_to_cdof. It is also easy to obtain
         celem_to_celem = AE_to_cface * AE_to_cface^T. This is mostly sufficient. What remains, and it is not hard,
         is to consider the AE matrices returned by \b ElementIPMatrix as "coarse" element matrices. Then, SAAMGe
         can be called and it produces further partitionings, MISes, and levels. The MISes on all further levels
         recover faces, that are unions of the initial IP faces, that are disjoint in terms of dofs on all levels,
         a setting initially established via the introduction of the IP formulation.

    The returned structure must be freed by the caller.
*/
agg_partitioning_relations_t *
nonconf_create_partitioning(const agg_partitioning_relations_t& agg_part_rels_nonconf,
                            const interp_data_t& interp_data_nonconf);

/**
    Returns agglomerate matrices for the interior penalty formulation.

    \a interp_data_nonconf is the one filled in through one of the routines in this module
    containing sparse or dense local (on agglomerates) matrices of fine or coarse scale.
    This class simply collects the pieces to obtain a fine or coarse scale IP matrix,
    or a Schur complement, on the agglomerate, or the faces of the agglomerate,
    respecting the local ordering of the dofs.

    The decision on which matrices to obtain and return depends on the availability and type
    of the information in \a interp_data_nonconf.

    The returned matrix must be freed by the caller.
*/
mfem::Matrix *nonconf_AE_matrix(const interp_data_t& interp_data_nonconf, int elno);

/**
    Returns agglomerate matrices for the interior penalty formulation.

    \a interp_data_nonconf is the one filled in through one of the routines in this module
    containing sparse or dense local (on agglomerates) matrices of fine or coarse scale.
    This class simply collects the pieces to obtain a fine or coarse scale IP matrix,
    or a Schur complement, on the agglomerate, or the faces of the agglomerate,
    respecting the local ordering of the dofs.

    The decision on which matrices to obtain and return depends on the availability and type
    of the information in \a interp_data_nonconf.

    This is to be used in the construction of a standard SAAMGe hierarchy, where the same agglomerates as the ones
    for the IP method are used during the first coarsening. Note that the IP method "breaks" the spaces along
    the agglomerates' faces.

    Overall, this (together with \b nonconf_create_partitioning) allows using SAAMGe on any of
    the IP problems formulated in this module:
        - coarse or fine
        - condensed or not

    XXX: Only agglomerate matrices are provided and no actual element matrices, since there are no
         element matrices available, whose assembly might provide the agglomerate matrices of interest.
*/
class ElementIPMatrix : public ElementMatrixProvider
{
public:
    ElementIPMatrix(const agg_partitioning_relations_t& agg_part_rels,
                    const interp_data_t& interp_data_nonconf) :
        ElementMatrixProvider(agg_part_rels),
        interp_data_nonconf(interp_data_nonconf)
    {
        is_geometric = false;
    }

    virtual mfem::Matrix *GetMatrix(int elno, bool& free_matr) const
    {
        SA_ASSERT(false); // Makes no sense, since effectively there is no element matrices,
                          // but only AE matrices.
        free_matr = true;
        return new mfem::DenseMatrix;
    }

    virtual mfem::Matrix *BuildAEStiff(int elno) const
    {
        return nonconf_AE_matrix(interp_data_nonconf, elno);
    }
private:
    const interp_data_t& interp_data_nonconf;
};

} // namespace saamge

#endif // _NONCONF_HPP
