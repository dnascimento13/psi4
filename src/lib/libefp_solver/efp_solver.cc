/*
 * EFP solver
 */ 

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility> 
             
#include <libmints/mints.h>
#include <psifiles.h>
#include <physconst.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.h>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>
#include <liboptions/python.h>
#include <psifiles.h>
#include <libmints/multipolesymmetry.h>
#include <psi4-dec.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "efp_solver.h"
#include "../../libefp/libefp/src/efp.h"

using namespace boost;
using namespace std;
using namespace psi;

// TODO: change allocated memory to shared pointers and ditch the deletes

namespace psi { namespace efp {
EFP::EFP(Options& options): options_(options) 
{
    common_init();
}

EFP::~EFP(){
       efp_shutdown(efp_);
}

static int string_compare(const void *a, const void *b)
{
       const char *s1 = *(const char **)a;
       const char *s2 = *(const char **)b;

       return strcasecmp(s1, s2);
}

char *EFP::make_name_list()
{
	size_t len = 0;

	for (int i = 0; i < nfrag_; i++) {
		std::string name = options_["FRAGS"][i].to_string();
		len += name.length() + 1;
	}

	char *names = new char[len], *ptr = names;

	for (int i = 0; i < nfrag_; i++) {
		std::string name = options_["FRAGS"][i].to_string();
		strcpy(ptr, name.c_str());
		ptr += strlen(name.c_str());
		*ptr++ = '\n';
	}

	*(--ptr) = '\0';
	ptr = names;

	while (*ptr) {
		*ptr = tolower(*ptr);
		ptr++;
	}

	return names;
}

/*
 * This function constructs the list of library potential data files from
 * the list of fragment names.
 *
 * For each unique fragment: if fragment name contains an _l suffix append
 * fraglib_path prefix and remove _l suffix. Otherwise append userlib_path
 * prefix. Add ".efp" extension in both cases.
 */
char *EFP::make_potential_file_list(const char *name_list, const char *fraglib_path,
		const char *userlib_path)
{
	const char **unique = new const char *[nfrag_];
	char *names = new char[strlen(name_list)], *names_ptr = names;
	strcpy(names, name_list);

	for (int i = 0; i < nfrag_; i++) {
		unique[i] = names_ptr;
		names_ptr = strchr(names_ptr, '\n');
		if (names_ptr)
			*names_ptr++ = '\0';
	}

	std::qsort(unique, nfrag_, sizeof(char *), string_compare);

	int n_unique = 1;

	for (int i = 1; i < nfrag_; i++) {
		if (strcmp(unique[i - 1], unique[i])) {
			unique[n_unique] = unique[i];
			n_unique++;
		}
	}

	size_t len = 0;

	for (int i = 0; i < n_unique; i++) {
		printf("%s\n", unique[i]);
		const char *name = unique[i];
		size_t n = strlen(name);

		if (n > 2 && strcmp(name + n - 2, "_l") == 0)
			len += strlen(fraglib_path) + n + 4;
		else
			len += strlen(userlib_path) + n + 6;
	}

	char *files = new char[len], *ptr = files;

	for (int i = 0; i < n_unique; i++) {
		const char *name = unique[i];
		size_t n = strlen(name);

		if (n > 2 && strcmp(name + n - 2, "_l") == 0) {
			strcat(strcpy(ptr, fraglib_path), "/");
			strcat(strncat(ptr, name, n - 2), ".efp");
			ptr += strlen(fraglib_path) + n + 3;
			*ptr++ = '\n';
		}
		else {
			strcat(strcpy(ptr, userlib_path), "/");
			strcat(strcat(ptr, name), ".efp");
			ptr += strlen(userlib_path) + n + 5;
			*ptr++ = '\n';
		}
	}

	*(--ptr) = '\0';
	delete[] unique;
	delete[] names;
	return files;
}

void EFP::common_init() {
       enum efp_result res;
       int print = options_.get_int("PRINT");

       struct efp_opts opts;
       memset(&opts, 0, sizeof(struct efp_opts));

       elst_enabled_ = options_.get_bool("EFP_ELST");
       pol_enabled_  = options_.get_bool("EFP_POL");
       disp_enabled_ = options_.get_bool("EFP_DISP");
       exch_enabled_ = options_.get_bool("EFP_EXCH");

       std::string dertype = options_.get_str("DERTYPE");
       do_grad_ = false;
       if (dertype == "FIRST")
           do_grad_ = true;

       if (elst_enabled_)
               opts.terms |= EFP_TERM_ELEC;
       if (pol_enabled_)
               opts.terms |= EFP_TERM_POL;
       if (disp_enabled_)
               opts.terms |= EFP_TERM_DISP;
       if (exch_enabled_)
               opts.terms |= EFP_TERM_XR;

       std::string elst_damping = options_.get_str("EFP_ELST_DAMPING");
       std::string disp_damping = options_.get_str("EFP_DISP_DAMPING");

       if (elst_damping == "SCREEN") {
               opts.elec_damp = EFP_ELEC_DAMP_SCREEN;
       } else if (elst_damping == "OVERLAP") {
               opts.elec_damp = EFP_ELEC_DAMP_OVERLAP;
       } else if (elst_damping == "OFF") {
               opts.elec_damp = EFP_ELEC_DAMP_OFF;
       }

       if (disp_damping == "TT") {
           opts.disp_damp = EFP_DISP_DAMP_TT;
       } else if (disp_damping == "OVERLAP") {
           opts.disp_damp = EFP_DISP_DAMP_OVERLAP;
       } else if (disp_damping == "OFF") {
           opts.disp_damp = EFP_DISP_DAMP_OFF;
       }

       nfrag_ = options_["FRAGS"].size();
       molecule_ = Process::environment.molecule();

       std::string psi_data_dir = Process::environment("PSIDATADIR");
       std::string fraglib_path = psi_data_dir + "/fraglib";

       char *frag_names = make_name_list();
       char *file_names = make_potential_file_list(frag_names, fraglib_path.c_str(), ".");

       if ((res = efp_init(&efp_, &opts, file_names, frag_names))) {
               fprintf(outfile, "%s", efp_result_to_string(res));
               throw PsiException("efp",__FILE__,__LINE__);
       }

       delete[] file_names;
       delete[] frag_names;

       fprintf(outfile, "  ==> Calculation Information <==\n\n");
       fprintf(outfile, "  Electrostatics damping: %12s\n", elst_damping.c_str());

       if (disp_enabled_)
               fprintf(outfile, "  Dispersion damping:     %12s\n", disp_damping.c_str());

       fprintf(outfile, "\n");
}

// Provid list of coordinates of quantum mechanical atoms
void EFP::SetQMAtoms(){
// TODO: extend molecule class and coordentry class to separate qm and efp atoms
}

void EFP::SetGeometry(){
       enum efp_result res;
       fprintf(outfile, "\n\n");
       fprintf(outfile, "%s", efp_banner());
       fprintf(outfile, "\n\n");

       fprintf(outfile, "  ==> Geometry <==\n\n");

       double *coords = NULL;

       molecule_->print();
       if (molecule_->nfragments() != nfrag_)
               throw InputException("Molecule doesn't have FRAGS number of fragments.", "FRAGS", nfrag_, __FILE__, __LINE__);

	// array of coordinates, 9 numbers for each fragment - first three atoms
	coords = new double[9 * nfrag_];
	double * pcoords = coords;

       for (int i = 0; i < nfrag_; i++) {
               std::vector<int> realsA;
               realsA.push_back(i);
               std::vector<int> ghostsA;
               for (int j = 0; j < nfrag_; j++) {
                               if (i != j)
                                               ghostsA.push_back(j);
               }
               boost::shared_ptr<Molecule> monomerA = molecule_->extract_subsets(realsA, ghostsA);
               monomerA->print();
               monomerA->print_in_bohr();

               int natomA = 0;
               for (int n=0; n<monomerA->natom(); n++)
                       if (monomerA->Z(n))
                               natomA++;

               if (natomA != 3)
                       throw InputException("Fragment doesn't have three coordinate triples.", "natomA", natomA, __FILE__, __LINE__);
       
               SharedMatrix xyz = SharedMatrix (new Matrix("Fragment Cartesian Coordinates(x,y,z)", monomerA->natom(), 3));
               double** xyzp = xyz->pointer();
       
               for (int j = 0; j < monomerA->natom(); j++) {
                       if (monomerA->Z(j)) {
                               *pcoords++ = xyzp[j][0] = monomerA->x(j);
                               *pcoords++ = xyzp[j][1] = monomerA->y(j);
                               *pcoords++ = xyzp[j][2] = monomerA->z(j);
                       }
               }

        fprintf(outfile, "%s\n",  options_["FRAGS"][i].to_string().c_str());
        xyz->print();
       }

       if ((res = efp_set_coordinates(efp_, EFP_COORD_TYPE_POINTS, coords))) {
               fprintf(outfile, "%s", efp_result_to_string(res));
                throw PsiException("efp",__FILE__,__LINE__);
       }
} // end of SetGeometry()

// this function returns a shared matrix containing the efp contribution to the potential
// felt by qm atoms in an scf procedure.
boost::shared_ptr<Matrix> EFP::modify_Fock() {

    // get number of multipoles
    int * n_multipole = (int*)malloc(4*sizeof(int));
    enum efp_result err = efp_get_multipole_count(efp_,n_multipole);
    if ( err != EFP_RESULT_SUCCESS ) {
        throw PsiException("libefp failed to return number of multipoles",__FILE__,__LINE__);
    }

    // workspace for efp_get_multipoles.
    //double ** xyz = (double**)malloc(4*sizeof(double*));
    //double **   z = (double**)malloc(4*sizeof(double*));
    //for (int i = 0; i < 4; i++) {
    //    xyz[i] = (double*)malloc(3*n_multipole[i]*sizeof(double));
    //    for (int j = 0; j < 3*n_multipole[i]; j++) xyz[i][j] = 0.0;
    //}
    //z[0] = (double*)malloc(n_multipole[0]*sizeof(double));
    //z[1] = (double*)malloc(3*n_multipole[1]*sizeof(double));
    //z[2] = (double*)malloc(6*n_multipole[2]*sizeof(double));
    //z[3] = (double*)malloc(10*n_multipole[3]*sizeof(double));

    //for (int j = 0; j < n_multipole[0]; j++)    z[0][j] = 0.0;
    //for (int j = 0; j < 3*n_multipole[1]; j++)  z[1][j] = 0.0;
    //for (int j = 0; j < 6*n_multipole[2]; j++)  z[2][j] = 0.0;
    //for (int j = 0; j < 10*n_multipole[3]; j++) z[3][j] = 0.0;

    // get multipoles from libefp
    // dipoles stored as     x,y,z
    // quadrupoles stored as xx,yy,zz,xy,xz,yz
    // octupoles stored as   xxx,yyy,zzz,xxy,xxz,xyy,yyz,xzz,yzz,xyz
    //
    // err = efp_get_grag_atoms - for atom charges
    // err = efp_get_multipole_values - for electrostatics multipones
    // err = efp_get_induced_dipole_values - for polarization induced dipoles
    // err = efp_get_induced_dipole_conj_values - for polarization induced dipoles
    //
    // induced dipoles
    //
    // int n_id;
    // check_fail(efp_get_induced_dipole_count(impl_->efp, &n_id));
    // double *xyz_id = new double[n_id * 3];
    // check_fail(efp_get_induced_dipole_coordinates(impl_->efp, xyz_id));
    // double *id = new double[n_id * 3];
    // check_fail(efp_get_induced_dipole_values(impl_->efp, id));
    // double *idt = new double[n_id * 3];
    // check_fail(efp_get_induced_dipole_conj_values(impl_->efp, idt));
    //
    // take avarage of id and idt, 0.5*(id+idt)

    // get electrostatic potential at each point returned in the xyz array
    // TODO: need this function

    // grab matrix factory from wavefunction
    boost::shared_ptr<Wavefunction> wfn         = Process::environment.wavefunction();
    boost::shared_ptr<MatrixFactory> matrix     = wfn->matrix_factory();
    boost::shared_ptr<IntegralFactory> integral = wfn->integral();

    // generate multipole integrals:
    // 
    // they will be ordered as follows in the vector, multipoles
    // x, y, z, 
    // xx, xy, xz, yy, yz, zz, 
    // xxx, xxy, xxz, xyy, xyz, xzz, yyy, yyz, yzz, zzz
    // 
    boost::shared_ptr<OneBodySOInt> mult3 ( integral->so_multipoles(3) );
    boost::shared_ptr<MultipoleSymmetry> multsym ( new MultipoleSymmetry(3,molecule_,integral,matrix) );
    std::vector<boost::shared_ptr<Matrix> > multipoles = multsym->create_matrices("Multipole: ");
    mult3->compute(multipoles);

    // arrays to map our multipole ordering to Ilya's
    int mapq[6]  = { 0, 3, 4, 1, 5, 2};
    int mapo[10] = { 0, 3, 4, 5, 9, 7, 1, 6, 8, 2};

    // dot multipoles with multipole integrals.  the result goes into V
    // TODO: need this function
    boost::shared_ptr<Matrix> V = matrix->create_shared_matrix("EFP V contribution");

    // free workspace memory needed by libefp
    //free(n_multipole);
    //for (int i = 0; i < 4; i++) {
    //    free(xyz[i]);
    //    free(z[i]);
    //}
    //free(xyz);
    //free(z);
   
    return V;
}

// compute efp contribution to scf energy
double EFP::scf_energy_update() {
    double efp_energy;
    efp_get_wavefunction_dependent_energy(efp_, &efp_energy);
    return efp_energy;
}

// compute efp energy components and/or gradient
void EFP::Compute() {
       enum efp_result res;
        double *grad = NULL;

       /* Main EFP computation routine */
       if ((res = efp_compute(efp_, do_grad_ ? 1 : 0))) {
               fprintf(outfile, "%s", efp_result_to_string(res));
                throw PsiException("efp",__FILE__,__LINE__);
       }

       struct efp_energy energy;

       if ((res = efp_get_energy(efp_, &energy))) {
               fprintf(outfile, "%s", efp_result_to_string(res));
                throw PsiException("efp",__FILE__,__LINE__);
       }

       if (do_grad_) {
                       grad = new double[6 * nfrag_];
                       if ((res = efp_get_gradient(efp_, nfrag_, grad))) {
                               fprintf(outfile, "%s", efp_result_to_string(res));
                                throw PsiException("efp",__FILE__,__LINE__);
                       }

                       fprintf(outfile, "  ==> EFP Gradient <==\n\n");

                       double *pgrad = grad;
                       for (int i = 0; i < nfrag_; i++) {
                                       for (int j = 0; j < 6; j++) {
                                                       fprintf(outfile, "%14.6lf", *pgrad++);
                                       }
                                       fprintf(outfile, "\n");
                       }
                       fprintf(outfile, "\n");

                       SharedMatrix smgrad(new Matrix("EFP Gradient", nfrag_, 6)); //
                       double ** psmgrad = smgrad->pointer();
                       pgrad = grad;
                       for (int i = 0; i < nfrag_; i++) {
                                       for (int jj = 0; jj < 6; jj++) {
                                               psmgrad[i][jj] = *pgrad++;
                                       }
                       }

                       psi::Process::environment.set_gradient(smgrad);
                       smgrad->print();
       }

    fprintf(outfile, "  ==> Energetics <==\n\n");

    fprintf(outfile, "  Electrostatics Energy = %24.16f [H] %s\n", energy.electrostatic, elst_enabled_ ? "*" : "");
    fprintf(outfile, "  Polarization Energy =   %24.16f [H] %s\n", energy.polarization, pol_enabled_ ? "*" : "");
    fprintf(outfile, "  Dispersion Energy =     %24.16f [H] %s\n", energy.dispersion, disp_enabled_ ? "*" : "");
    fprintf(outfile, "  Exchange Energy =       %24.16f [H] %s\n", energy.exchange_repulsion, exch_enabled_ ? "*" : "");
    fprintf(outfile, "  Total Energy =          %24.16f [H] %s\n", energy.total, "*");

    Process::environment.globals["EFP ELST ENERGY"] = energy.electrostatic;
    Process::environment.globals["EFP POL ENERGY"] = energy.polarization;
    Process::environment.globals["EFP DISP ENERGY"] = energy.dispersion;
    Process::environment.globals["EFP EXCH ENERGY"] = energy.exchange_repulsion;
    Process::environment.globals["CURRENT ENERGY"] = energy.total;


}

// Get number of fragments
int EFP::get_frag_count(void) {
  return nfrag_;
}

// Get number of atoms in a fragment
int EFP::get_frag_atom_count(int frag_idx) {
  int n=0;
  enum efp_result res;
  res = efp_get_frag_atom_count(efp_, frag_idx, &n);
  return n;
}

// Get atomic numbers of atoms in a fragment
double *EFP::get_frag_atom_Z(int frag_idx) {

  if (frag_idx >= nfrag_) return NULL;

  int frag_natom = get_frag_atom_count(frag_idx);
  if (frag_natom == 0)
    return NULL;

  struct efp_atom atoms[frag_natom];
  enum efp_result res;
  res = efp_get_frag_atoms(efp_, frag_idx, frag_natom, atoms);

  if (res != EFP_RESULT_SUCCESS)
    return NULL;

  double *frag_atom_Z = new double[frag_natom];
  for (int i=0; i<frag_natom; ++i)
    frag_atom_Z[i] = atoms[i].znuc;

  return frag_atom_Z;
}

// Get masses of atoms in a fragment
double *EFP::get_frag_atom_mass(int frag_idx) {
  
  if (frag_idx >= nfrag_) return NULL;
  
  int frag_natom = get_frag_atom_count(frag_idx);
  
  struct efp_atom atoms[frag_natom];
  enum efp_result res;
  res = efp_get_frag_atoms(efp_, frag_idx, frag_natom, atoms);
  if (res != EFP_RESULT_SUCCESS) return NULL;
  
  double *frag_atom_mass = new double[frag_natom];
  for (int i=0; i<frag_natom; ++i)
    frag_atom_mass[i] = atoms[i].mass;
  
  return frag_atom_mass;
}

// Fetch COM for fragment from libefp coordinates
double *EFP::get_com(int frag_idx) {
  if (frag_idx >= nfrag_) return NULL;
  
  double *xyzabc = new double [6*nfrag_];
  efp_get_coordinates(efp_, nfrag_, xyzabc);
  double *com = new double[3];
  com[0] = xyzabc[6*frag_idx+0];
  com[1] = xyzabc[6*frag_idx+1];
  com[2] = xyzabc[6*frag_idx+2];
  
  return com;
}

}

} // End namespaces

