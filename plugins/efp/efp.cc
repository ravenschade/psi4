#include <libplugin/plugin.h>
#include <psi4-dec.h>
#include <libparallel/parallel.h>
#include <liboptions/liboptions.h>
#include <libmints/mints.h>
#include <libpsio/psio.hpp>

#include <efp.h>

INIT_PLUGIN

using namespace boost;

namespace psi{ namespace efp {

static int string_compare(const void *a, const void *b)
{
	const char *s1 = *(const char **)a;
	const char *s2 = *(const char **)b;

	return strcasecmp(s1, s2);
}

/* this is from libefp/efpmd/parse.c */
static char **make_potential_file_list(const char **frag_name,
				       const char *fraglib_path,
				       const char *userlib_path)
{
	/* This function constructs the list of library potential data files.
	 * For each unique fragment if fragment name contains an _l suffix
	 * append fraglib_path prefix and remove _l suffix. Otherwise append
	 * userlib_path prefix. Add .efp extension in both cases. */

	int n_frag = 0;

	while (frag_name[n_frag])
		n_frag++;

	const char **unique = new const char*[n_frag];

	for (int i = 0; i < n_frag; i++)
		unique[i] = frag_name[i];

	qsort(unique, n_frag, sizeof(char *), string_compare);

	int n_unique = 1;

	for (int i = 1; i < n_frag; i++)
		if (strcasecmp(unique[i - 1], unique[i])) {
			unique[n_unique] = unique[i];
			n_unique++;
		}

	char **list = new char*[n_unique + 1];

	for (int i = 0; i < n_unique; i++) {
		const char *name = unique[i];
		size_t len = strlen(name);
		char *path;

		if (len > 2 && name[len - 2] == '_' && name[len - 1] == 'l') {
			path = new char[strlen(fraglib_path) + len + 4];
			strcat(strcpy(path, fraglib_path), "/");
			strcat(strncat(path, name, len - 2), ".efp");
		}
		else {
			path = new char[strlen(userlib_path) + len + 6];
			strcat(strcpy(path, userlib_path), "/");
			strcat(strcat(path, name), ".efp");
		}

		list[i] = path;
	}

	list[n_unique] = NULL;
	delete[] unique;
	return list;
}

extern "C" 
int read_options(std::string name, Options& options)
{
	if (name == "EFP"|| options.read_globals()) {
		/*- The amount of information printed to the output file. -*/
		options.add_int("PRINT", 1);
		/*- Type of EFP simulation. One of single point (``SP``), gradient
		(``GRAD``), conjugate gradient geometry optimization (``CG``),
		molecular dynamics in microcanonical ensemble (``NVE``), or
		molecular dynamics in canonical ensemble (``NVT``). This
		specification will probably be moved to energy(), grad(), opt(),
		etc. eventually. -*/
		//        options.add_str("EFP_TYPE", "SP", "SP GRAD CG NVE NVT");
		/*- Do include electrostatics energy term in EFP computation? -*/
		options.add_bool("EFP_ELST", true);
		/*- Do include polarization energy term in EFP computation? -*/
		options.add_bool("EFP_POL", true);
		/*- Do include dispersion energy term in EFP computation? -*/
		options.add_bool("EFP_DISP", true);
		/*- Do include exchange repulsion energy term in EFP computation? -*/
		options.add_bool("EFP_EXCH", true);
			/*- Electrostatic damping type. ``SCREEN`` is a damping formula
		based on screen group in the EFP potential. ``OVERLAP`` is
		damping that computes charge penetration energy. -*/
		options.add_str("EFP_ELST_DAMPING", "SCREEN", "SCREEN OVERLAP OFF");
		/*- Dispersion damping type. ``TT`` is a damping formula by
		Tang and Toennies. ``OVERLAP`` is overlap-based dispersion damping. -*/
		options.add_str("EFP_DISP_DAMPING", "OVERLAP", "TT OVERLAP OFF");
		/*- Names of fragment files corresponding to molecule subsets.
		This is temporary until better EFP input geometry parsing is implemented. -*/
		options.add("FRAGS", new ArrayType());
		/* Do EFP gradient. */
		options.add_bool("EFP_GRADIENT", false);
	}

	return true;
}

extern "C" 
PsiReturnType efp(Options& options)
{
	enum efp_result res;
	int print = options.get_int("PRINT");

	struct efp_opts opts;
	memset(&opts, 0, sizeof(struct efp_opts));

	bool elst_enabled = options.get_bool("EFP_ELST");
	bool pol_enabled  = options.get_bool("EFP_POL");
	bool disp_enabled = options.get_bool("EFP_DISP");
	bool exch_enabled = options.get_bool("EFP_EXCH");

	bool do_grad = options.get_bool("EFP_GRADIENT");

	if (elst_enabled)
		opts.terms |= EFP_TERM_ELEC;
	if (pol_enabled)
		opts.terms |= EFP_TERM_POL;
	if (disp_enabled)
		opts.terms |= EFP_TERM_DISP;
	if (exch_enabled)
		opts.terms |= EFP_TERM_XR;

	double *coords = NULL, *pcoords, *grad = NULL;

	std::string elst_damping = options.get_str("EFP_ELST_DAMPING");
	std::string disp_damping = options.get_str("EFP_DISP_DAMPING");

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

	int nfrag = options["FRAGS"].size();
	boost::shared_ptr<Molecule> molecule = Process::environment.molecule();

	char **frag_name = new char*[nfrag + 1]; /* NULL-terminated */

	for(int i=0; i<nfrag; i++) {
		std::string name = options["FRAGS"][i].to_string();
		frag_name[i] = new char[name.length() + 1];
		strcpy(frag_name[i], name.c_str());
		for (char *p = frag_name[i]; *p; p++)
			*p = tolower(*p);
	}
	frag_name[nfrag] = NULL;

	std::string psi_data_dir = Process::environment("PSIDATADIR");
	std::string frag_lib_path = psi_data_dir + "/fraglib";

	char **potential_file_list = make_potential_file_list((const char **)frag_name,
		frag_lib_path.c_str(), frag_lib_path.c_str());

	struct efp *efp;

	// XXX
	//struct efp_callbacks;
	//efp_callbacks.get_electron_density_field = &get_electron_field;

	// signature
	//typedef enum efp_result (*efp_electron_density_field_fn)(int n_pt,
	//	 const double *xyz, double *field, void *user_data);

	//  Will be passed as a last parameter to
	//    efp_callbacks::get_electron_density_field.
	//efp_callbacks.get_electron_density_field_user_data = NULL;

	// place &efp_callbacks instead of NULL
	if ((res = efp_init(&efp, &opts, NULL, (const char **)potential_file_list, (const char **)frag_name))) {
		fprintf(outfile, efp_result_to_string(res));
		goto fail;
	}

	fprintf(outfile, "\n\n");
	fprintf(outfile, efp_banner());
	fprintf(outfile, "\n\n");

	fprintf(outfile, "  ==> Geometry <==\n\n");

	molecule->print();
	if (molecule->nfragments() != nfrag)
		throw InputException("Molecule doesn't have FRAGS number of fragments.", "FRAGS", nfrag, __FILE__, __LINE__);

	// array of coordinates, 9 numbers for each fragment - first three atoms
	coords = new double[9 * nfrag], pcoords = coords;

	for (int i = 0; i < nfrag; i++) {
		std::vector<int> realsA;
		realsA.push_back(i);
		std::vector<int> ghostsA;
		for (int j = 0; j < nfrag; j++) {
				if (i != j)
						ghostsA.push_back(j);
		}
		boost::shared_ptr<Molecule> monomerA = molecule->extract_subsets(realsA, ghostsA);
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

        fprintf(outfile, "%s\n", frag_name[i]);
    	xyz->print();
	}

	if ((res = efp_set_coordinates_2(efp, coords))) {
		fprintf(outfile, efp_result_to_string(res));
		goto fail;
	}

  	fprintf(outfile, "  ==> Calculation Information <==\n\n");

	fprintf(outfile, "  Electrostatics damping: %12s\n", elst_damping.c_str());

	if (disp_enabled)
		fprintf(outfile, "  Dispersion damping:     %12s\n", disp_damping.c_str());

	fprintf(outfile, "\n");

	/* Main EFP computation routine */
	if ((res = efp_compute(efp, do_grad ? 1 : 0))) {
		fprintf(outfile, efp_result_to_string(res));
		goto fail;
	}

	struct efp_energy energy;

	if ((res = efp_get_energy(efp, &energy))) {
		fprintf(outfile, efp_result_to_string(res));
		goto fail;
	}

	if (do_grad) {
			grad = new double[6 * nfrag];
			double *pgrad = grad;
			if ((res = efp_get_gradient(efp, 6 * nfrag, grad))) {
				fprintf(outfile, efp_result_to_string(res));
				goto fail;
			}

			fprintf(outfile, "  ==> EFP Gradient <==\n\n");

			for (int i = 0; i < nfrag; i++) {
					for (int j = 0; j < 6; j++) {
							fprintf(outfile, "%14.6lf", *pgrad++);
					}
					fprintf(outfile, "\n");
			}
			fprintf(outfile, "\n");
	}

    fprintf(outfile, "  ==> Energetics <==\n\n");

    fprintf(outfile, "  Electrostatics Energy = %24.16f [H] %s\n", energy.electrostatic, elst_enabled ? "*" : "");
    fprintf(outfile, "  Polarization Energy =   %24.16f [H] %s\n", energy.polarization, pol_enabled ? "*" : "");
    fprintf(outfile, "  Dispersion Energy =     %24.16f [H] %s\n", energy.dispersion, disp_enabled ? "*" : "");
    fprintf(outfile, "  Exchange Energy =       %24.16f [H] %s\n", energy.exchange_repulsion, exch_enabled ? "*" : "");
    fprintf(outfile, "  Total Energy =          %24.16f [H] %s\n", energy.total, "*");

    Process::environment.globals["EFP ELST ENERGY"] = energy.electrostatic;
    Process::environment.globals["EFP POL ENERGY"] = energy.polarization;
    Process::environment.globals["EFP DISP ENERGY"] = energy.dispersion;
    Process::environment.globals["EFP EXCH ENERGY"] = energy.exchange_repulsion;
    Process::environment.globals["CURRENT ENERGY"] = energy.total;

fail:
	for (int i = 0; frag_name[i]; i++)
		delete[] frag_name[i];
	delete[] frag_name;

	for (int i = 0; potential_file_list[i]; i++)
			delete[] potential_file_list[i];
	delete[] potential_file_list;

	delete[] coords;
	delete[] grad;

	efp_shutdown(efp);
    return res ? Failure : Success;
}

}} // End namespaces
