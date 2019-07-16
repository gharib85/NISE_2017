#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "omp.h"
#include "types.h"
#include "NISE_subs.h"
#include "polar.h"
#include "calc_2DES.h"
#include <stdarg.h>
#include "mpi.h"

// Print results to the corresponding files
void print2D(char* filename, float** arrR, float** arrI, t_non* non, int sampleCount) {
    FILE* out = fopen(filename, "w");
    for (int t1 = 0; t1 < non->tmax1; t1 += non->dt1) {
        const int t2 = non->tmax2;
        for (int t3 = 0; t3 < non->tmax3; t3 += non->dt3) {
            arrR[t3][t1] /= sampleCount;
            arrI[t3][t1] /= sampleCount;
            fprintf(out, "%f %f %f %e %e\n", t1 * non->deltat, t2 * non->deltat, t3 * non->deltat,
                arrR[t3][t1], arrI[t3][t1]);
        }
    }
    fclose(out);
}

void calculateWorkset(t_non* non, int** workset, int* sampleCount, int* clusterCount) {
    // Open clustering file if necessary
    FILE* Cfile;
    if (non->cluster != -1) {
        Cfile = fopen("Cluster.bin", "rb");
        if (Cfile == NULL) {
            printf("Cluster option was activated but no Cluster.bin file provided.\n");
            printf("Please, provide cluster file or remove Cluster keyword from\n");
            printf("input file.\n");
            exit(0);
        }
    }
    *clusterCount = 0;

    // Determine number of samples
    const int totalSampleCount = (non->length - non->tmax1 - non->tmax2 - non->tmax3 - 1) / non->sample + 1;
    if (totalSampleCount > 0) {
        printf("Making %d samples!\n", totalSampleCount);
    }
    else {
        printf("Insufficient data to calculate spectrum.\n");
        printf("Please, lower max times or provide longer\n");
        printf("trajectory.\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    if (non->end == 0) non->end = totalSampleCount;
    *sampleCount = non->end - non->begin;
    if (*sampleCount == 0) {
        // Avoid dividing by zero
        *sampleCount = 1;
    }

    // Allocate workset array
    *workset = calloc(2 * 21 * (*sampleCount), sizeof(int)); // two integers per work item (sample + polDir), 21 polDirs per sample

    // Loop over samples, fill work set array of things to do
    int currentWorkItem = 0;
    for(int currentSample = non->begin; currentSample < non->end; currentSample++) {
        // Check clustering
        if(non->cluster != -1) {
            int tj = currentSample * non->sample + non->tmax1;

            int currentCluster;
            if (read_cluster(non, tj, &currentCluster, Cfile) != -1) {
                printf("Cluster trajectory file to short, could not fill buffer!!!\n");
                printf("ITIME %d\n", tj);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            // If we do not need to consider current cluster, skip calculation
            if(non->cluster != currentCluster) {
                log_item("Skipping sample %d, incorrect cluster!\n", currentSample);
                (*sampleCount)--;
                continue;
            }

            (*clusterCount)++;
        }

        // Set work items
        for(int molPol = 0; molPol < 21; molPol++) {
            (*workset)[currentWorkItem * 2] = currentSample;
            (*workset)[currentWorkItem * 2 + 1] = molPol;
            currentWorkItem++;
        }
    }

    // Now we have a workset array with all sample/molPol combinations that need to be calculated.
    // Furthermore, sampleCount is adjusted to reflect the actual number of samples to consider when clustering

    if (non->cluster != -1) {
        fclose(Cfile);
    }
}

void calc_2DES(t_non* non, int parentRank, int parentSize, int subRank, int subSize, MPI_Comm subComm, MPI_Comm rootComm) {
    // Start by determining the work to be done, make an array of samples/poldir to simulate
    // and distribute those statically over all processors
    int clusterCount = 0, sampleCount = 0, * workset, worksetSize = 0;
    if(parentRank == 0) {
        // Master process calculates the work items to be performed
        int* fullWorkset;
        calculateWorkset(non, &fullWorkset, &sampleCount, &clusterCount);
        log_item("Begin sample: %d, End sample: %d.\n", non->begin, non->end);

        // Distribute work, each process does as many items as the others (static decomposition)
        MPI_Bcast(&clusterCount, 1, MPI_INT, 0, MPI_COMM_WORLD);
        MPI_Bcast(&sampleCount, 1, MPI_INT, 0, MPI_COMM_WORLD);

        int totalWorkItems = 21 * sampleCount;
        worksetSize = (totalWorkItems - 1) / parentSize + 1; // Quick ceil

        // Resize the work array to be easy to distribute over all processes, fill with -1 for jobs to ignore
        fullWorkset = realloc(fullWorkset, 2 * worksetSize * parentSize * sizeof(int));
        for (int i = totalWorkItems * 2; i < 2 * worksetSize * parentSize; i++) fullWorkset[i] = -1;

        MPI_Bcast(&worksetSize, 1, MPI_INT, 0, MPI_COMM_WORLD);
        workset = malloc(2 * worksetSize * sizeof(int));
        MPI_Scatter(fullWorkset, 2 * worksetSize, MPI_INT, workset, 2 * worksetSize, MPI_INT, 0, MPI_COMM_WORLD);

        free(fullWorkset);
    } else {
        // Fix non settings
        const int totalSampleCount = (non->length - non->tmax1 - non->tmax2 - non->tmax3 - 1) / non->sample + 1;
        if (non->end == 0) non->end = totalSampleCount;

        // Receive information on workset
        MPI_Bcast(&clusterCount, 1, MPI_INT, 0, MPI_COMM_WORLD);
        MPI_Bcast(&sampleCount, 1, MPI_INT, 0, MPI_COMM_WORLD);

        MPI_Bcast(&worksetSize, 1, MPI_INT, 0, MPI_COMM_WORLD);
        workset = malloc(2 * worksetSize * sizeof(int));
        MPI_Scatter(NULL, 2 * worksetSize, MPI_INT, workset, 2 * worksetSize, MPI_INT, 0, MPI_COMM_WORLD);
    }

    // Initialize each process base variables
    float* pol = 0; /* Currently dummy vector that can be used to change coordinate system in the future */ // RO
    const int nn2 = non->singles * (non->singles + 1) / 2;
    float* Hamil_i_e = calloc(nn2, sizeof(float));

    // Frequency shifts
    float shift1 = (non->max1 + non->min1) / 2;
    if(parentRank == 0) printf("Frequency shift %f.\n", shift1);
    non->shifte = shift1;
    non->shiftf = 2 * shift1;

    // Arrays where the result is stored, these will be reduced (summed) at the end!

    // 2D response function parallel
    float** rrIpar = (float**) calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIpar = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** rrIIpar = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIIpar = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    // 2D response function perpendicular
    float** rrIper = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIper = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** rrIIper = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIIper = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    // 2D response function cross
    float** rrIcro = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIcro = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** rrIIcro = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE
    float** riIIcro = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // REDUCE


    // These arrays are initialized here and only read in the loops
    float** lt_gb_se = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // RO
    float** lt_ea = (float**)calloc2D(non->tmax3, non->tmax1, sizeof(float), sizeof(float*)); // RO

    for (int t1 = 0; t1 < non->tmax1; t1++) {
        for (int t3 = 0; t3 < non->tmax3; t3++) {
            lt_gb_se[t3][t1] = (float) exp(-(double)(t1 + t3) * non->deltat / (2 * non->lifetime));
            lt_ea[t3][t1] = (float) exp(-(double)(t1 + t3) * non->deltat / (2 * non->lifetime));
        }
    }

    // Open the various files, these are readonly per process so safe for concurrent use
    FILE* H_traj = fopen(non->energyFName, "rb");
    if (H_traj == NULL) {
        if(parentRank == 0) printf("Hamiltonian file not found!\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    FILE* mu_traj = fopen(non->dipoleFName, "rb");
    if (mu_traj == NULL) {
        if (parentRank == 0) printf("Dipole file %s not found!\n", non->dipoleFName);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* Open file for fluctuating anharmonicities and sequence transition dipoles if needed */
    FILE* A_traj, *mu2_traj;
    if (non->anharmonicity == 0 && (!strcmp(non->technique, "2DUVvis") || (!strcmp(non->technique, "EAUVvis")) || (!
            strcmp(non->technique, "noEAUVvis")) || (!strcmp(non->technique, "GBUVvis")) || (!strcmp(
            non->technique, "SEUVvis")))) {
        A_traj = fopen(non->anharFName, "rb");
        if (A_traj == NULL) {
            if (parentRank == 0) printf("Anharmonicity file %s not found!\n", non->anharFName);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        mu2_traj = fopen(non->overdipFName, "rb");
        if (mu2_traj == NULL) {
            if (parentRank == 0) printf("Overtone dipole file %s not found!\n", non->overdipFName);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    /* Read coupling */
    float* mu_xyz = calloc(3 * non->singles, sizeof(float)); // This is readonly inside the loops
    if (!strcmp(non->hamiltonian, "Coupling")) {
        FILE* C_traj = fopen(non->couplingFName, "rb");
        if (C_traj == NULL) {
            if (parentRank == 0) printf("Coupling file not found!\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        if (read_He(non, Hamil_i_e, C_traj, -1) != 1) {
            if (parentRank == 0) printf("Coupling trajectory file to short, could not fill buffer!!!\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        fclose(C_traj);

        for (int x = 0; x < 3; x++) {
            if (read_mue(non, mu_xyz + non->singles * x, mu_traj, 0, x) != 1) {
                if (parentRank == 0) printf("Dipole trajectory file to short, could not fill buffer!!!\n");
                if (parentRank == 0) printf("ITIME %d %d\n", 0, x);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }
    }

    // From now on we'll do the calculations
    for (int currentWorkItem = 0; currentWorkItem < worksetSize; currentWorkItem++) {
        int currentSample = workset[2 * currentWorkItem];
        int molPol = workset[2 * currentWorkItem + 1];

        if (currentSample == -1 || molPol == -1) continue;

        /* Log time */
        time_t timeSampleStart;
        time(&timeSampleStart);
        //log_item("Starting sample %d\n", currentSample); TODO fix the logging in parallel
        printf("Starting sample %d, molPol %d\n", currentSample, molPol);

        /* Calculate 2DIR response */
        int tj = currentSample * non->sample + non->tmax1;
        int tk = tj + non->tmax2;
        int px[4];
        polar(px, molPol);

        // Allocate arrays
        float* Anh = calloc(non->singles, sizeof(float));
        float* over = calloc(non->singles, sizeof(float));

        float* leftrr = calloc(non->singles, sizeof(float));
        float* leftri = calloc(non->singles, sizeof(float));
        float** leftnr = (float**)calloc2D(non->tmax1, non->singles, sizeof(float), sizeof(float*));
        float** leftni = (float**)calloc2D(non->tmax1, non->singles, sizeof(float), sizeof(float*));
        float** rightrr = (float**)calloc2D(non->tmax1, non->singles, sizeof(float), sizeof(float*));
        float** rightri = (float**)calloc2D(non->tmax1, non->singles, sizeof(float), sizeof(float*));
        float* rightnr = calloc(non->singles, sizeof(float));
        float* rightni = calloc(non->singles, sizeof(float));

        float* mut2 = calloc(non->singles, sizeof(float));
        float* mut3r = calloc(non->singles, sizeof(float));
        float* mut3i = calloc(non->singles, sizeof(float));
        float* mut4 = calloc(non->singles, sizeof(float));

        float* fr = calloc(nn2, sizeof(float));
        float* fi = calloc(nn2, sizeof(float));
        float** ft1r = (float**)calloc2D(non->tmax1, nn2, sizeof(float), sizeof(float*));
        float** ft1i = (float**)calloc2D(non->tmax1, nn2, sizeof(float), sizeof(float*));

        // Read information
        mureadE(non, mut2, tj, px[1], mu_traj, mu_xyz, pol);

        /* Ground state bleach (GB) kI and kII */
        for (int t1 = 0; t1 < non->tmax1; t1++) {
            /* Read dipoles at time 0 */
            mureadE(non, leftnr[t1], tj - t1, px[0], mu_traj, mu_xyz, pol);
            clearvec(leftni[t1], non->singles);

            /* Propagate */
            for (int tm = 0; tm < t1; tm++) {
                /* Read Hamiltonian */
                if (read_He(non, Hamil_i_e, H_traj, tj - t1 + tm) != 1) {
                    printf("Hamiltonian trajectory file to short, could not fill buffer!!!\n");
                    exit(1);
                }

                if (non->propagation == 1) {
                    propagate_vec_coupling_S(
                        non, Hamil_i_e, leftnr[t1], leftni[t1], non->ts, 1
                    );
                } else if (non->propagation == 0) {
                    propagate_vec_DIA_S(non, Hamil_i_e, leftnr[t1], leftni[t1], 1);
                }

            }
        }

        float* t1nr = calloc(non->tmax1, sizeof(float));
        float* t1ni = calloc(non->tmax1, sizeof(float));

        for (int t1 = 0; t1 < non->tmax1; t1++) {
            t1nr[t1] = 0, t1ni[t1] = 0;
            for (int i = 0; i < non->singles; i++) {
                t1nr[t1] += mut2[i] * leftnr[t1][i];
                t1ni[t1] += mut2[i] * leftni[t1][i];
            }
        }

        free(mut2);

        /* Combine with evolution during t3 */
        mureadE(non, mut3r, tk, px[2], mu_traj, mu_xyz, pol);
        clearvec(mut3i, non->singles);
        for (int t3 = 0; t3 < non->tmax3; t3++) {
            int tl = tk + t3;
            mureadE(non, mut4, tl, px[3], mu_traj, mu_xyz, pol);
            float t3nr = 0, t3ni = 0;
            for (int i = 0; i < non->singles; i++) {
                t3nr += mut4[i] * mut3r[i];
                t3ni += mut4[i] * mut3i[i];
            }
            /* Calculate GB contributions */
            if ((!strcmp(non->technique, "GBUVvis")) || (!strcmp(non->technique, "2DUVvis")) || (!strcmp(
                non->technique, "noEAUVvis"))) {
                for (int t1 = 0; t1 < non->tmax1; t1++) {
                    float polWeight = polarweight(0, molPol) * lt_gb_se[t3][t1];
                    rrIpar[t3][t1] -= (t3ni * t1nr[t1] - t3nr * t1ni[t1]) * polWeight;
                    riIpar[t3][t1] -= (t3nr * t1nr[t1] + t3ni * t1ni[t1]) * polWeight;
                    rrIIpar[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIpar[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                    polWeight = polarweight(1, molPol) * lt_gb_se[t3][t1];
                    rrIper[t3][t1] -= (t3ni * t1nr[t1] - t3nr * t1ni[t1]) * polWeight;
                    riIper[t3][t1] -= (t3nr * t1nr[t1] + t3ni * t1ni[t1]) * polWeight;
                    rrIIper[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIper[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                    polWeight = polarweight(2, molPol) * lt_gb_se[t3][t1];
                    rrIcro[t3][t1] -= (t3ni * t1nr[t1] - t3nr * t1ni[t1]) * polWeight;
                    riIcro[t3][t1] -= (t3nr * t1nr[t1] + t3ni * t1ni[t1]) * polWeight;
                    rrIIcro[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIcro[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                }
            }

            /* Read Hamiltonian */
            if (read_He(non, Hamil_i_e, H_traj, tl) != 1) {
                printf("Hamiltonian trajectory file to short, could not fill buffer!!!\n");
                exit(1);
            }

            /* Propagate */
            if (non->propagation == 1) propagate_vec_coupling_S(non, Hamil_i_e, mut3r, mut3i, non->ts, 1);
            if (non->propagation == 0) propagate_vec_DIA_S(non, Hamil_i_e, mut3r, mut3i, 1);
        }

        /* Stimulated emission (SE) */
        /* Calculate evolution during t2 */
        mureadE(non, leftrr, tj, px[1], mu_traj, mu_xyz, pol);
        clearvec(leftri, non->singles);
        for (int t2 = 0; t2 < non->tmax2; t2++) {
            int tm = tj + t2;
            if (read_He(non, Hamil_i_e, H_traj, tm) != 1) {
                printf("Hamiltonian trajectory file to short, could not fill buffer!!!\n");
                exit(1);
            }


            propagate_vec_DIA(non, Hamil_i_e, leftrr, leftri, 1);
            for (int t1 = 0; t1 < non->tmax1; t1++) {
                propagate_vec_DIA(
                    non, Hamil_i_e, leftnr[t1], leftni[t1], 1
                );
            }
        }

        /* Read dipole for third interaction */
        mureadE(non, mut3r, tk, px[2], mu_traj, mu_xyz, pol);

        if ((!strcmp(non->technique, "EAUVvis")) || (!strcmp(non->technique, "2DUVvis"))) {
            if (non->anharmonicity == 0) {
                read_over(non, over, mu2_traj, tk, px[2]);
            }
            /* T2 propagation ended store vectors needed for EA */
            dipole_double(non, mut3r, leftrr, leftri, fr, fi, over);
            for (int t1 = 0; t1 < non->tmax1; t1++) {
                dipole_double(
                    non, mut3r, leftnr[t1], leftni[t1],
                    ft1r[t1], ft1i[t1], over
                );
            }

            memcpy(rightrr[0], leftnr[0], non->tmax1 * non->singles * sizeof(float));
            memcpy(rightri[0], leftni[0], non->tmax1* non->singles * sizeof(float));
            for (int i = 0; i < non->tmax1 * non->singles; i++) rightri[0][i] = -rightri[0][i];

            memcpy(rightnr, leftrr, non->singles * sizeof(float));
            memcpy(rightni, leftri, non->singles * sizeof(float));
            for (int i = 0; i < non->singles; i++) rightni[i] = -rightni[i];
        }

        clearvec(mut3i, non->singles);

        /* Calculate right side of nonrephasing diagram */
        float t3nr = 0, t3ni = 0;
        for (int i = 0; i < non->singles; i++) {
            t3nr += leftrr[i] * mut3r[i];
            t3ni -= leftri[i] * mut3r[i];
        }

        /* Calculate right side of rephasing diagram */
        float* t1rr = calloc(non->tmax1, sizeof(float));
        float* t1ri = calloc(non->tmax1, sizeof(float));

        for (int t1 = 0; t1 < non->tmax1; t1++) {
            t1rr[t1] = 0, t1ri[t1] = 0;
            for (int i = 0; i < non->singles; i++) {
                t1rr[t1] += leftnr[t1][i] * mut3r[i];
                t1ri[t1] -= leftni[t1][i] * mut3r[i];
            }
        }

        /* Combine with evolution during t3 */
        for (int t3 = 0; t3 < non->tmax3; t3++) {
            int tl = tk + t3;
            mureadE(non, mut4, tl, px[3], mu_traj, mu_xyz, pol);

            /* Calculate left side of nonrephasing diagram */
            float t3rr = 0, t3ri = 0;
            for (int i = 0; i < non->singles; i++) {
                t3rr += mut4[i] * leftrr[i];
                t3ri += mut4[i] * leftri[i];
            }

            /* Calculate left side of rephasing diagram */
            for (int t1 = 0; t1 < non->tmax1; t1++) {
                t1nr[t1] = 0, t1ni[t1] = 0;
                for (int i = 0; i < non->singles; i++) {
                    t1nr[t1] += leftnr[t1][i] * mut4[i];
                    t1ni[t1] += leftni[t1][i] * mut4[i];
                }
            }

            /* Calculate Response */
            if ((!strcmp(non->technique, "SEUVvis")) || (!strcmp(non->technique, "2DUVvis")) || (!strcmp(
                non->technique, "noEAUVvis"))) {
                for (int t1 = 0; t1 < non->tmax1; t1++) {
                    float polWeight = polarweight(0, molPol) * lt_gb_se[t3][t1];
                    rrIpar[t3][t1] -= (t3rr * t1ri[t1] + t3ri * t1rr[t1]) * polWeight;
                    riIpar[t3][t1] -= (t3rr * t1rr[t1] - t3ri * t1ri[t1]) * polWeight;
                    rrIIpar[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIpar[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                    polWeight = polarweight(1, molPol) * lt_gb_se[t3][t1];
                    rrIper[t3][t1] -= (t3rr * t1ri[t1] + t3ri * t1rr[t1]) * polWeight;
                    riIper[t3][t1] -= (t3rr * t1rr[t1] - t3ri * t1ri[t1]) * polWeight;
                    rrIIper[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIper[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                    polWeight = polarweight(2, molPol) * lt_gb_se[t3][t1];
                    rrIcro[t3][t1] -= (t3rr * t1ri[t1] + t3ri * t1rr[t1]) * polWeight;
                    riIcro[t3][t1] -= (t3rr * t1rr[t1] - t3ri * t1ri[t1]) * polWeight;
                    rrIIcro[t3][t1] -= (t3ni * t1nr[t1] + t3nr * t1ni[t1]) * polWeight;
                    riIIcro[t3][t1] -= (t3nr * t1nr[t1] - t3ni * t1ni[t1]) * polWeight;
                }
            }


            /* Do Propagation */
            /* Read Hamiltonian */
            if (read_He(non, Hamil_i_e, H_traj, tl) != 1) {
                printf("Hamiltonian trajectory file to short, could not fill buffer!!!\n");
                exit(1);
            }

            /* Propagate left side rephasing */
            if (non->propagation == 1) {
                propagate_vec_coupling_S(non, Hamil_i_e, leftrr, leftri, non->ts, 1);
            } else if (non->propagation == 0) {
                propagate_vec_DIA_S(non, Hamil_i_e, leftrr, leftri, 1);
            }

            /* Propagate left side nonrephasing */
            for (int t1 = 0; t1 < non->tmax1; t1++) {
                if (non->propagation == 0) {
                    propagate_vec_DIA_S(
                        non, Hamil_i_e, leftnr[t1], leftni[t1], 1
                    );
                } else if (non->propagation == 1) {
                    propagate_vec_coupling_S(
                        non, Hamil_i_e, leftnr[t1], leftni[t1], non->ts, 1
                    );
                }
            }
        }

        if ((!strcmp(non->technique, "EAUVvis")) || (!strcmp(non->technique, "2DUVvis"))) {
            /* Excited state absorption (EA) */
            /* Combine with evolution during t3 */
            for (int t3 = 0; t3 < non->tmax3; t3++) {
                int tl = tk + t3;
                /* Read Dipole t4 */
                mureadE(non, mut4, tl, px[3], mu_traj, mu_xyz, pol);
                if (non->anharmonicity == 0) {
                    read_over(non, over, mu2_traj, tl, px[3]);
                }

                /* Multiply with the last dipole */
                dipole_double_last(non, mut4, fr, fi, leftrr, leftri, over);
                for (int t1 = 0; t1 < non->tmax1; t1++) {
                    dipole_double_last(
                        non, mut4, ft1r[t1], ft1i[t1], leftnr[t1], leftni[t1], over
                    );
                }

                /* Calculate EA response */
                for (int t1 = 0; t1 < non->tmax1; t1++) {
                    float rrI = 0, riI = 0, rrII = 0, riII = 0;
                    for (int i = 0; i < non->singles; i++) {
                        rrI += leftri[i] * rightrr[t1][i] + leftrr[i] * rightri[t1][i];
                        riI += leftrr[i] * rightrr[t1][i] - rightri[t1][i] * leftri[i];

                        rrII += rightnr[i] * leftni[t1][i] + rightni[i] * leftnr[t1][i];
                        riII += rightnr[i] * leftnr[t1][i] - rightni[i] * leftni[t1][i];
                    }

                    float polWeight = polarweight(0, molPol) * lt_ea[t3][t1];
                    rrIpar[t3][t1] += rrI * polWeight;
                    riIpar[t3][t1] += riI * polWeight;
                    rrIIpar[t3][t1] += rrII * polWeight;
                    riIIpar[t3][t1] += riII * polWeight;
                    polWeight = polarweight(1, molPol) * lt_ea[t3][t1];
                    rrIper[t3][t1] += rrI * polWeight;
                    riIper[t3][t1] += riI * polWeight;
                    rrIIper[t3][t1] += rrII * polWeight;
                    riIIper[t3][t1] += riII * polWeight;
                    polWeight = polarweight(2, molPol) * lt_ea[t3][t1];
                    rrIcro[t3][t1] += rrI * polWeight;
                    riIcro[t3][t1] += riI * polWeight;
                    rrIIcro[t3][t1] += rrII * polWeight;
                    riIIcro[t3][t1] += riII * polWeight;
                }

                /* Read Hamiltonian */
                if (read_He(non, Hamil_i_e, H_traj, tl) != 1) {
                    printf("Hamiltonian trajectory file to short, could not fill buffer!!!\n");
                    exit(1);
                }

                /* Propagate vectors left */
                if (non->anharmonicity == 0) {
                    read_A(non, Anh, A_traj, tl);
                }

                /* Propagate */
                if (non->propagation == 0) {
                    float* Urs = calloc(non->singles * non->singles, sizeof(float));
                    float* Uis = calloc(non->singles * non->singles, sizeof(float));

                    int* Rs = calloc(non->singles * non->singles, sizeof(int));
                    int* Cs = calloc(non->singles * non->singles, sizeof(int));

                    // bug? Cs and Rs seems to be exchanged
                    int elements = time_evolution_mat(non, Hamil_i_e, Urs, Uis, Cs, Rs, non->ts);
                    if (currentSample == non->begin && molPol == 0 && t3 == 0) {
                        printf("Sparse matrix efficiency: %f pct.\n",
                               (1 - (1.0 * elements / (non->singles * non->singles))) * 100);
                        printf("Present truncation %f.\n",
                               non->thres / ((double) non->deltat * icm2ifs * (double) twoPi / non->ts * (non->deltat *
                                   icm2ifs * twoPi / non->ts)));
                        printf("Suggested truncation %f.\n", 0.001);
                    }

                    // Key parallel loop 1
                    // Initial step, former t1=-1
                    propagate_double_sparce(
                        non, Urs, Uis, Rs, Cs, fr, fi, elements, non->ts, Anh
                    );

                    int t1; // MSVC can't deal with C99 declarations inside a for with OpenMP
                    /*#pragma omp parallel for \
                        shared(non, Anh, Urs, Uis, Rs, Cs, ft1r, ft1i) \
                        schedule(static, 1)*/

                    for(t1 = 0; t1 < non->tmax1; t1++) {
                        propagate_double_sparce(
                            non, Urs, Uis, Rs, Cs, ft1r[t1],
                            ft1i[t1], elements, non->ts, Anh
                        );
                    }

                    // Propagate vectors right
                    // Key parallel loop 2
                    // Initial step
                    propagate_vec_DIA_S(non, Hamil_i_e, rightnr, rightni, -1);

                    for(t1 = 0; t1 < non->tmax1; t1++) {
                        propagate_vec_DIA_S(
                            non, Hamil_i_e, rightrr[t1], rightri[t1], -1
                        );
                    }

                    free(Urs), free(Uis), free(Rs), free(Cs);
                }
                else if(non->propagation == 1) {
                    // Key parallel loop 1
                    // Initial step
                    propagate_vec_coupling_S_doubles(
                        non, Hamil_i_e, fr, fi, non->ts, Anh
                    );

                    int t1;
                    /*#pragma omp parallel for \
                        shared(non,Hamil_i_e,Anh,ft1r,ft1i) \
                        schedule(static, 1)*/

                    for (t1 = 0; t1 < non->tmax1; t1++) {
                        propagate_vec_coupling_S_doubles(
                            non, Hamil_i_e, ft1r[t1], ft1i[t1], non->ts, Anh
                        );
                    }

                    // Key parallel loop 2
                    // Initial step
                    propagate_vec_coupling_S(
                        non, Hamil_i_e, rightnr, rightni, non->ts, -1
                    );

                    for (t1 = 0; t1 < non->tmax1; t1++) {
                        propagate_vec_coupling_S(
                            non, Hamil_i_e, rightrr[t1], rightri[t1], non->ts, -1
                        );
                    }
                }
            }
        }

        free(leftrr), free(leftri), free2D((void**) leftnr), free2D((void**) leftni);
        free2D((void**) rightrr), free2D((void**) rightri), free(rightnr), free(rightni);
        free(t1rr), free(t1ri), free(t1nr), free(t1ni);
        free(mut3r);
        free(mut3i);
        free(mut4);
        free(Anh), free(over);
        free(fr), free(fi);
        free2D((void**) ft1r), free2D((void**) ft1i);

        time_t timeSampleEnd;
        time(&timeSampleEnd);
        char* timeText = time_diff(timeSampleStart, timeSampleEnd);
        //log_item("Finished sample %d, %s\n", currentSample, timeText);
        printf("Finished sample %d, molPol %d in %s", currentSample, molPol, timeText);
        free(timeText);
    }

    // Reduce calculation results, in a tiered approach to save network bandwidth
    int reduceArraySize = non->tmax3 * non->tmax1;
    float** reductionArrays[12] = {
        rrIpar, riIpar, rrIIpar, riIIpar, rrIper, riIper, rrIIper, riIIper, rrIcro, riIcro, rrIIcro, riIIcro
    };

    // Reduce at small scope
    if(subRank == 0) { // parent will do in-place reduce to save memory
        for (int i = 0; i < 12; i++) {
            MPI_Reduce(MPI_IN_PLACE, reductionArrays[i][0], reduceArraySize, MPI_FLOAT, MPI_SUM, 0, subComm);
        }
    } else {
        for (int i = 0; i < 12; i++) {
            MPI_Reduce(reductionArrays[i][0], NULL, reduceArraySize, MPI_FLOAT, MPI_SUM, 0, subComm);
        }
    }

    // Reduce at global scope
    if (parentRank == 0) { // parent will do in-place reduce to save memory
        for (int i = 0; i < 12; i++) {
            MPI_Reduce(MPI_IN_PLACE, reductionArrays[i][0], reduceArraySize, MPI_FLOAT, MPI_SUM, 0, rootComm);
        }
    }
    else if(subRank == 0) {
        for (int i = 0; i < 12; i++) {
            MPI_Reduce(reductionArrays[i][0], NULL, reduceArraySize, MPI_FLOAT, MPI_SUM, 0, rootComm);
        }
    }

    /* The calculation is finished, lets write output */
    if (parentRank == 0) {
        log_item("Finished Calculating Response!\nWriting to file\n");

        printf("Samples %d\n", sampleCount);

        if (non->cluster != -1) {
            printf("Of %d samples %d belonged to cluster %d.\n", sampleCount, clusterCount, non->cluster);
        }

        /* Close Files */
        fclose(mu_traj), fclose(H_traj);
        if ((!strcmp(non->technique, "2DUVvis")) || (!strcmp(non->technique, "GBUVvis")) || (!
            strcmp(non->technique, "SEUVvis")) || (!strcmp(non->technique, "EAUVvis")) || (!strcmp(
                non->technique, "noEAUVvis"))) {
            if (non->anharmonicity == 0) {
                fclose(mu2_traj), fclose(A_traj);
            }
        }

        /* Print 2D */
        print2D("RparI.dat", rrIpar, riIpar, non, sampleCount);
        print2D("RparII.dat", rrIIpar, riIIpar, non, sampleCount);
        print2D("RperI.dat", rrIper, riIper, non, sampleCount);
        print2D("RperII.dat", rrIIper, riIIper, non, sampleCount);
        print2D("RcroI.dat", rrIcro, riIcro, non, sampleCount);
        print2D("RcroII.dat", rrIIcro, riIIcro, non, sampleCount);

        printf("----------------------------------------\n");
        printf(" 2DIR calculation succesfully completed\n");
        printf("----------------------------------------\n\n");
    }

    /* Free memory for 2D calculation */
    free2D((void**)rrIpar), free2D((void**)riIpar);
    free2D((void**)rrIIpar), free2D((void**)riIIpar);
    free2D((void**)rrIper), free2D((void**)riIper);
    free2D((void**)rrIIper), free2D((void**)riIIper);
    free2D((void**)rrIcro), free2D((void**)riIcro);
    free2D((void**)rrIIcro), free2D((void**)riIIcro);
    free(mu_xyz);
    free2D((void**)lt_gb_se);
    free2D((void**)lt_ea);
    free(workset);
}
