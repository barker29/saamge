/*! \file

    SAAMGE: smoothed aggregation element based algebraic multigrid hierarchies
            and solvers.

    Copyright (c) 2015, Lawrence Livermore National Security,
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

    This file was contributed by Umberto Villa.
*/

#include <fstream>
#include <mfem.hpp>
#include "InversePermeabilityFunction.hpp"

void InversePermeabilityFunction::SetNumberCells(int Nx_, int Ny_, int Nz_)
{
    Nx = Nx_;
    Ny = Ny_;
    Nz = Nz_;
}

void InversePermeabilityFunction::SetMeshSizes(double hx_, double hy_, 
					       double hz_)
{
    hx = hx_;
    hy = hy_;
    hz = hz_;
}

void InversePermeabilityFunction::SetConstantInversePermeability(double ipx, 
								 double ipy, 
								 double ipz)
{
    int compSize = Nx*Ny*Nz;
    int size = 3*compSize;
    inversePermeability = new double [size];
    double *ip = inversePermeability;
    // double * end = inversePermeability + size;

    for(int i(0); i < compSize; ++i)
    {
    	ip[i] = ipx;
    	ip[i+compSize] = ipy;
    	ip[i+2*compSize] = ipz;
    }

}

void InversePermeabilityFunction::ReadPermeabilityFile(
    const std::string fileName)
{
    std::ifstream permfile(fileName.c_str());

    if(!permfile.is_open())
    	mfem_error("File do not exists");

    inversePermeability = new double [3*Nx*Ny*Nz];
    double *ip = inversePermeability;
    double tmp;
    for(int l = 0; l < 3; l++)
    {
	for (int k = 0; k < Nz; k++)
	{
	    for (int j = 0; j < Ny; j++)
	    {
		for (int i = 0; i < Nx; i++)
		{
		    permfile >> *ip;
		    *ip = 1./(*ip);
		    ip++;
		}
		for (int i = 0; i < 60-Nx; i++)
		    permfile >> tmp; // skip unneeded part
	    }
	    for (int j = 0; j < 220-Ny; j++)
		for (int i = 0; i < 60; i++)
		    permfile >> tmp;  // skip unneeded part
	}

	if (l < 2) // if not processing Kz, skip unneeded part
	    for (int k = 0; k < 85-Nz; k++)
		for (int j = 0; j < 220; j++)
		    for (int i = 0; i < 60; i++)
			permfile >> tmp;
    }

}

void InversePermeabilityFunction::InversePermeability(const Vector & x, 
						      Vector & val)
{
    val.SetSize(x.Size());

    unsigned int i=0,j=0,k=0;

    switch(orientation)
    {
    case NONE:
	i = Nx-1-(int)floor(x[0]/hx/(1.+3e-16));
	j = (int)floor(x[1]/hy/(1.+3e-16));
	k = Nz-1-(int)floor(x[2]/hz/(1.+3e-16));
	break;
    case XY:
	i = Nx-1-(int)floor(x[0]/hx/(1.+3e-16));
	j = (int)floor(x[1]/hy/(1.+3e-16));
	k = npos;
	break;
    case XZ:
	i = Nx-1-(int)floor(x[0]/hx/(1.+3e-16));
	j = npos;
	k = Nz-1-(int)floor(x[2]/hz/(1.+3e-16));
	break;
    case YZ:
	i = npos;
	j = (int)floor(x[1]/hy/(1.+3e-16));
	k = Nz-1-(int)floor(x[2]/hz/(1.+3e-16));
	break;
    default:
	mfem_error("InversePermeabilityFunction::InversePermeability");
    }

    val[0] = inversePermeability[Ny*Nx*k + Nx*j + i];
    val[1] = inversePermeability[Ny*Nx*k + Nx*j + i + Nx*Ny*Nz];

    if(orientation == NONE)
	val[2] = inversePermeability[Ny*Nx*k + Nx*j + i + 2*Nx*Ny*Nz];

}

double InversePermeabilityFunction::PermeabilityXComponent(Vector &x)
{
    unsigned int i = Nx-1-(int)floor(x[0]/hx/(1.+3e-16));
    unsigned int j = (int)floor(x[1]/hy/(1.+3e-16));
    unsigned int k = Nz-1-(int)floor(x[2]/hz/(1.+3e-16));
    
    double out = 1.0/inversePermeability[Ny*Nx*k + Nx*j + i];
    // std::cout << "perm = " << out << std::endl;
    return out;
}

void InversePermeabilityFunction::NegativeInversePermeability(const Vector & x,
							      Vector & val)
{
    InversePermeability(x,val);
    val *= -1.;
}


void InversePermeabilityFunction::Permeability(const Vector & x, Vector & val)
{
    InversePermeability(x,val);
    for (double * it = val.GetData(), *end = val.GetData()+val.Size(); it != end; ++it )
	(*it) = 1./ (*it);
}

void InversePermeabilityFunction::PermeabilityTensor(const Vector & x, DenseMatrix & val)
{
    Vector tmp(val.Size());
    Permeability(x,tmp);
    val = 0.0;
    for (int i=0; i<val.Size(); ++i)
	val.Elem(i,i) = tmp(i);
}

double InversePermeabilityFunction::Norm2InversePermeability(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return val.Norml2();
}

double InversePermeabilityFunction::Norm1InversePermeability(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return val.Norml1();
}

double InversePermeabilityFunction::NormInfInversePermeability(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return val.Normlinf();
}

double InversePermeabilityFunction::InvNorm2(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return 1./val.Norml2();
}

double InversePermeabilityFunction::InvNorm1(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return 1./val.Norml1();
}

double InversePermeabilityFunction::InvNormInf(const Vector & x)
{
    Vector val(3);
    InversePermeability(x,val);
    return 1./val.Normlinf();
}


void InversePermeabilityFunction::ClearMemory()
{
    delete[] inversePermeability;
}

int InversePermeabilityFunction::Nx(60);
int InversePermeabilityFunction::Ny(220);
int InversePermeabilityFunction::Nz(85);
double InversePermeabilityFunction::hx(20);
double InversePermeabilityFunction::hy(10);
double InversePermeabilityFunction::hz(2);
double * InversePermeabilityFunction::inversePermeability(NULL);
InversePermeabilityFunction::SliceOrientation InversePermeabilityFunction::orientation( 
    InversePermeabilityFunction::NONE );
int InversePermeabilityFunction::npos(-1);

