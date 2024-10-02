/*
 * Copyright (C) 2023 University of Bologna
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license.  See the LICENSE file for details.
 *
 * Authors: Alberto Dequino (alberto.dequino@unibo.it), Calin Diaconu (calin.diaconu@studio.unibo.it)
 */


#include "pulp_mhsa_fp32.h"
#include "pulp_matmul_fp32.h"
#include "pulp_train_utils_fp32.h"
#include "pulp_act_fp32.h"
#include <math.h>


//FORWARD
void pulp_mhsa_fp32_fw_cl(void *Mhsa_args) {
    // ======================================== DECLARATIONS ========================================
    struct Mhsa_args *mhsa_args = (struct Mhsa_args *) Mhsa_args;

    float *coeffDataWinQ = mhsa_args->coeff_in_q->data;         //  TRANSPOSE of Input Projection Weights for Query
    float *coeffDataWinK = mhsa_args->coeff_in_k->data;         //  TRANSPOSE of Input Projection Weights for Key
    float *coeffDataWinV = mhsa_args->coeff_in_v->data;         //  TRANSPOSE of Input Projection Weights for Value

    float *coeffBiasWinQ = mhsa_args->bias_in_q->data;          //  Input Projection Biases for Query
    float *coeffBiasWinK = mhsa_args->bias_in_k->data;          //  Input Projection Biases for Key
    float *coeffBiasWinV = mhsa_args->bias_in_v->data;          //  Input Projection Biases for Value

    float *coeffDataWout = mhsa_args->coeff_out->data;          //  Output Projection Weights (Already transposed from GM)
    float *attention_map = mhsa_args->attention_map->data;      //  Buffer saving the MHSA map before output projection
    float *outData = mhsa_args->output->data;                   //  Output sequence (Transposed, E x L)
    float *inputData = mhsa_args->input->data;                  //  Input vector (Transposed, E x L)
    float *temp = mhsa_args->temp_buffer;                       //  Support buffer used in the attention head loop
    float *softmax_buffer = mhsa_args->softmax_buffer->data;    //  Buffer containing the softmax results (necessary to save for backward pass)
    float *maxes = mhsa_args->maxes;                            //  Buffer containing the row-wise maxes in the softmax process
    float *sums = mhsa_args->sums;                              //  Buffer containing the row-wise exponential sums in the softmax process
    float *q = mhsa_args->q->data;                              //  Pointer to the first element of Q
    float *k = mhsa_args->k->data;                              //  Pointer to the first element of K
    float *v = mhsa_args->v->data;                              //  Pointer to the first element of V
    int n_heads = mhsa_args->n_heads;                           //  Number of heads used for MHSA

    int opt_matmul_type = mhsa_args->opt_matmul_type_fw;        //  Matmul type used

    int L = mhsa_args->input->H;                                //  Input/Output Sequence length    
    int E = mhsa_args->input->W;                                //  Input Sequence element size
    int F = mhsa_args->attention_map->W;                        //  Hidden dimension of attention (N. Heads * Head dimension)

    #ifdef DEBUG
    printf("\n~~~~~~~~~~~~~~~FORWARD PASS~~~~~~~~~~~~~~~\n\nPrinting the parameters: L-%d, E-%d, F-%d", L, E, F);
    #endif

    int H = F / n_heads;                                        //  Head dimension
    float scaling = q_rsqrt((float) H);                         //  Scaling factor to avoid vanishing gradients
    //float scaling = 1/sqrt(H);


    // ================================================== OP 1 ==================================================
    // Custom bias addition needed, since original operation is a linear layer, applied here as a matrix multiplication
    // ~~~~~~~~~~~~~~~~~~~ coeffDataWinQ [^T] -T-> temp [coeffDataWinQ]  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         (T0_q)
    // ~~~~~~~~~~~~~~~~~~~       E x F        -T->      F x E            ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ temp [coeffDataWinQ] @ inputData ->   q   ~~~~~~~~~~~~~~~~~~~~~~~         (M1_q)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         F x E        @   E x L   -> F x L ~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~ coeffDataWinK [^T] -T-> temp [coeffDataWinK]  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         (T0_k)
    // ~~~~~~~~~~~~~~~~~~~       E x F        -T->      F x E            ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ temp [coeffDataWinK] @ inputData ->   k   ~~~~~~~~~~~~~~~~~~~~~~~         (M1_k)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         F x E        @  E x L    -> F x L ~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~ coeffDataWinV [^T] -T-> temp [coeffDataWinV]  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         (T0_v)
    // ~~~~~~~~~~~~~~~~~~~       E x F        -T->      F x E            ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ temp [coeffDataWinV] @ inputData ->   v   ~~~~~~~~~~~~~~~~~~~~~~~         (M1_v)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~         F x E        @  E x L    -> F x L ~~~~~~~~~~~~~~~~~~~~~~~
    // TODO 0000: Maybe different key size (from q and v)

    // T0_q
    struct transp_args transp_args0_q;
    transp_args0_q.matrix = coeffDataWinQ;
    transp_args0_q.transp_matrix = temp;
    transp_args0_q.N = E;
    transp_args0_q.M = F;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args0_q);

    #ifdef DEBUG
    printf("\n\n\nT0_q result\n\ncoeffDataWinQ [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  transp_args0_q.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", transp_args0_q.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M1_q
    // Projecting input sequence into Q
    struct matMul_args matMul_args1_q;
    matMul_args1_q.A = temp;                                       //  F x E
    matMul_args1_q.B = inputData;                                  //  E x L
    matMul_args1_q.C = q;
    matMul_args1_q.N = F;
    matMul_args1_q.K = E;
    matMul_args1_q.M = L;
    matMul_args1_q.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args1_q);
    #else
    struct mm_manager_args man_args1_q;
    man_args1_q.mm_args = &matMul_args1_q;
    man_args1_q.layer_type = LAYER_LINEAR;
    man_args1_q.step_type = STEP_FW;
    man_args1_q.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args1_q);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM1_q result\n\ncoeffDataWinQ: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ",  matMul_args1_q.A[j]);
    }
    printf("\n");

    printf("\ninputData: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_q.B[j]);
    }
    printf("\n");

    printf("\nq: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_q.C[j]);
    }
    printf("\n\n");
    #endif

    // Bias_addition_q
    struct mm_bias_add_args mm_bias_add_args_q;
    mm_bias_add_args_q.mat = q;
    mm_bias_add_args_q.bias = coeffBiasWinQ;
    mm_bias_add_args_q.H = F;
    mm_bias_add_args_q.W = L;

    pi_cl_team_fork(NUM_CORES, mm_bias_add_transposed, &mm_bias_add_args_q);

    // T0_k
    struct transp_args transp_args0_k;
    transp_args0_k.matrix = coeffDataWinK;
    transp_args0_k.transp_matrix = temp;
    transp_args0_k.N = E;
    transp_args0_k.M = F;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args0_k);

    #ifdef DEBUG
    printf("\n\n\nT0_k result\n\ncoeffDataWinK [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  transp_args0_k.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", transp_args0_k.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M1_k
    // Projecting input sequence into K
    struct matMul_args matMul_args1_k;
    matMul_args1_k.A = temp;                                       //  F x E
    matMul_args1_k.B = inputData;                                  //  E x L
    matMul_args1_k.C = k;
    matMul_args1_k.N = F;
    matMul_args1_k.K = E;
    matMul_args1_k.M = L;
    matMul_args1_k.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args1_k);
    #else
    struct mm_manager_args man_args1_k;
    man_args1_k.mm_args = &matMul_args1_k;
    man_args1_k.layer_type = LAYER_LINEAR;
    man_args1_k.step_type = STEP_FW;
    man_args1_k.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args1_k);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM1_k result\n\ncoeffDataWinK: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ",  matMul_args1_k.A[j]);
    }
    printf("\n");

    printf("\ninputData: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_k.B[j]);
    }
    printf("\n");

    printf("\nk: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_k.C[j]);
    }
    printf("\n\n");
    #endif

    // Bias_addition_k
    struct mm_bias_add_args mm_bias_add_args_k;
    mm_bias_add_args_k.mat = k;
    mm_bias_add_args_k.bias = coeffBiasWinK;
    mm_bias_add_args_k.H = F;
    mm_bias_add_args_k.W = L;

    pi_cl_team_fork(NUM_CORES, mm_bias_add_transposed, &mm_bias_add_args_k);

    // T0_v
    struct transp_args transp_args0_v;
    transp_args0_v.matrix = coeffDataWinV;
    transp_args0_v.transp_matrix = temp;
    transp_args0_v.N = E;
    transp_args0_v.M = F;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args0_v);

    #ifdef DEBUG
    printf("\n\n\nT0_v result\n\ncoeffDataWinV [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  transp_args0_v.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", transp_args0_v.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M1_v
    // Projecting input sequence into V
    struct matMul_args matMul_args1_v;
    matMul_args1_v.A = temp;                                       //  F x E
    matMul_args1_v.B = inputData;                                  //  E x L
    matMul_args1_v.C = v;
    matMul_args1_v.N = F;
    matMul_args1_v.K = E;
    matMul_args1_v.M = L;
    matMul_args1_v.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args1_v);
    #else
    struct mm_manager_args man_args1_v;
    man_args1_v.mm_args = &matMul_args1_v;
    man_args1_v.layer_type = LAYER_LINEAR;
    man_args1_v.step_type = STEP_FW;
    man_args1_v.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args1_v);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM1_v result\n\ncoeffDataWinV: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ",  matMul_args1_v.A[j]);
    }
    printf("\n");

    printf("\ninputData: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_v.B[j]);
    }
    printf("\n");

    printf("\nv: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args1_v.C[j]);
    }
    printf("\n\n");
    #endif

    // Bias_addition_v
    struct mm_bias_add_args mm_bias_add_args_v;
    mm_bias_add_args_v.mat = v;
    mm_bias_add_args_v.bias = coeffBiasWinV;
    mm_bias_add_args_v.H = F;
    mm_bias_add_args_v.W = L;

    pi_cl_team_fork(NUM_CORES, mm_bias_add_transposed, &mm_bias_add_args_v);

    //  Cycle on the different heads
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ F -> H ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    for (int i = 0; i < n_heads; i++) {
        // ================================================== OP 3 ==================================================
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   k   -T-> temp [k ^ T] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~                    (T1)
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ H x L -T->    L x H     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ temp [k ^ T] @   q   -> softmax_buffer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~     (M2)
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    L x H     @ H x L ->      L x L     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        //  T1
        struct transp_args transp_args1;
        transp_args1.matrix = k + L * i * H;
        transp_args1.transp_matrix = temp;
        transp_args1.N = H;
        transp_args1.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args1);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T1 result\n\nk: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args1.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", transp_args1.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // M2
        // Multiply it with the i-th head's Q chunk
        struct matMul_args matMul_args2;
        matMul_args2.A = temp;
        matMul_args2.B = q + L * i * H;
        matMul_args2.C = softmax_buffer + i * L * L;
        matMul_args2.N = L;
        matMul_args2.K = H;
        matMul_args2.M = L;
        matMul_args2.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args2);
        #else
        struct mm_manager_args man_args2;
        man_args2.mm_args = &matMul_args2;
        man_args2.layer_type = LAYER_LINEAR;
        man_args2.step_type = STEP_FW;
        man_args2.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args2);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M2 result\n\ntemp: %d %d\n", i, L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ",  matMul_args2.A[j]);
        }
        printf("\n");

        printf("\nq: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args2.B[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args2.C[j]);
        }
        printf("\n\n");
        #endif


        // ================================================== OP 4 ==================================================
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer *= scalar ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~           L x L          ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        struct scalar_mul_args s_m_args;
        s_m_args.input = softmax_buffer + i * L * L;
        s_m_args.scalar = scaling;
        s_m_args.dim = L * L;

        #ifdef DEBUG
        printf("\n\n\nHead %d - scalar result\n\nsoftmax_buffer (BEFORE scaling): %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", s_m_args.input[j]);
        }
        printf("\n\n");
        #endif

        pi_cl_team_fork(NUM_CORES, pulp_scalar_mul_fp32_cl, &s_m_args);

        #ifdef DEBUG
        printf("\n\n\nsoftmax_buffer (AFTER scaling): %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", s_m_args.input[j]);
        }
        printf("\n\n");
        #endif


        // ================================================== OP 5 ==================================================
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer -T-> temp [softmax_buffer ^ T]  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   (T2)
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~      L x L     -T->         L x L              ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        //
        // ~~~~~~~~~~~~~~~~~~ temp [softmax_buffer ^ T] -SM-> softmax_buffer [softmax_buffer ^ T]  ~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~          L x L            -SM->               L x L                  ~~~~~~~~~~~~~~~~~~

        //  Due to the fact that we multiplied K * Qt instead of Q * Kt like in the original MHSA model, the current
        //  head buffer is transposed. To achieve the best experimental accuracy, the Softmax algorithm requires to compute
        //  row-wise max and sums, therefore it is necessary to transpose the current head buffer.
        // T2
        struct transp_args transp_args2;
        transp_args2.matrix = softmax_buffer + i * L * L;
        transp_args2.transp_matrix = temp;
        transp_args2.N = L;
        transp_args2.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args2);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T2 result\n\nsoftmax_buffer: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args2.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", transp_args2.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        //  Softmax algorithm
        struct softmax_args softmax_arg;
        softmax_arg.input_data = temp;
        softmax_arg.output_data = softmax_buffer + i * L * L;
        softmax_arg.maxes = maxes;
        softmax_arg.sums = sums;
        softmax_arg.H = L;
        softmax_arg.W = L;

        pulp_softmax_fp32_fw_cl(&softmax_arg);

        #ifdef DEBUG
        printf("\n\n\nHead %d - softmax result\n\ntemp: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  softmax_arg.input_data[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", softmax_arg.output_data[j]);
        }
        printf("\n\n");
        #endif


        // ================================================== OP 6 ==================================================
        // ~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer [softmax_buffer ^ T] -T-> temp [softmax_buffer] ~~~~~~~~~~~~~~~~~~~~~~     (T3)
        // ~~~~~~~~~~~~~~~~~~~~~~               L x L                 -T->        L x L          ~~~~~~~~~~~~~~~~~~~~~~

        // ~~~~~~~~~~~~~~~~~~~~~~   v   @ temp [softmax_buffer] -> attention_map ~~~~~~~~~~~~~~~~~~~~~~                     (M3)
        // ~~~~~~~~~~~~~~~~~~~~~~ H x L @        L x L          ->     H x L     ~~~~~~~~~~~~~~~~~~~~~~

        // T3
        //  Each head result has to be appended to the full attention map, to do so we require to store the current
        //  softmax buffer data following the H x L convention, therefore we need to transpose the memory buffer again.
        struct transp_args transp_args3;
        transp_args3.matrix = softmax_buffer + i * L * L;
        transp_args3.transp_matrix = temp;
        transp_args3.N = L;
        transp_args3.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args3);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T2 result\n\nsoftmax_buffer: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args3.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", transp_args3.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // M3
        struct matMul_args matMul_args3;
        matMul_args3.A = v + L * i * H;
        matMul_args3.B = temp;
        matMul_args3.C = attention_map + L * i * H;
        matMul_args3.N = H;
        matMul_args3.K = L;
        matMul_args3.M = L;
        matMul_args3.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args3);
        #else
        struct mm_manager_args man_args3;
        man_args3.mm_args = &matMul_args3;
        man_args3.layer_type = LAYER_LINEAR;
        man_args3.step_type = STEP_FW;
        man_args3.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args3);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M3 result\n\nv: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  matMul_args3.A[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args3.B[j]);
        }
        printf("\n");

        printf("\nattention_map: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args3.C[j]);
        }
        printf("\n\n");
        #endif
    }
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ H -> F ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    // ================================================== OP 7 ==================================================
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWout @ attention_map -> outData ~~~~~~~~~~~~~~~~~~~~~~       (M4)
    // ~~~~~~~~~~~~~~~~~~~~~~     E x F     @     F x L     ->  E x L  ~~~~~~~~~~~~~~~~~~~~~~

    // M4
    //  Final attention map projection
    struct matMul_args matMul_args4;
    matMul_args4.A = coeffDataWout;
    matMul_args4.B = attention_map;
    matMul_args4.C = outData;
    matMul_args4.N = E;
    matMul_args4.K = F;
    matMul_args4.M = L;
    matMul_args4.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args4);
    #else
    struct mm_manager_args man_args4;
    man_args4.mm_args = &matMul_args4;
    man_args4.layer_type = LAYER_LINEAR;
    man_args4.step_type = STEP_FW;
    man_args4.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args4);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM4 result\n\ncoeffDataWout: %d %d\n", E, F);
        for (int j=0; j<E*F; j++){
            if(!(j%(F))) printf("\n");
            printf("%.8f ",  matMul_args4.A[j]);
        }
        printf("\n");

        printf("\nq: %d %d\n", F, L);
        for (int j=0; j<F*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args4.B[j]);
        }
        printf("\n");

        printf("\noutData: %d %d\n", E, L);
        for (int j=0; j<E*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args4.C[j]);
        }
        printf("\n\n");
    #endif
}


//BACKWARD
void pulp_mhsa_fp32_bw_cl(void *Mhsa_args) {
    // ======================================== DECLARATIONS ========================================
    struct Mhsa_args *mhsa_args = (struct Mhsa_args *) Mhsa_args;

    float *coeffDataWinQ = mhsa_args->coeff_in_q->data; // F x E
    float *coeffDataWinK = mhsa_args->coeff_in_k->data; // F x E
    float *coeffDataWinV = mhsa_args->coeff_in_v->data; // F x E

    float *coeffDataWout = mhsa_args->coeff_out->data; // E x F
    float *attention_map = mhsa_args->attention_map->data; // F x L
    float *inputData = mhsa_args->input->data; // L x E
    float *temp = mhsa_args->temp_buffer; // Temporary buffer to save transposed matrices
    float *softmax_buffer = mhsa_args->softmax_buffer->data;
    float *sums = mhsa_args->sums;
    float *grad = mhsa_args->grad; // L x L

    int L = mhsa_args->input->H; // Input sequence length
    int E = mhsa_args->input->W; // Input sequence element size
    int F = mhsa_args->attention_map->W; // Attention block hidden size
    int n_heads = mhsa_args->n_heads; // Number of heads of the mhsa
    int H = F / n_heads;
    int opt_matmul_type = mhsa_args->opt_matmul_type_wg;

    float *q = mhsa_args->q->data;            // F x L
    float *k = mhsa_args->k->data;            // F x L
    float *v = mhsa_args->v->data;            // F x L

    float *q_diff = mhsa_args->q->diff;       // F x L
    float *k_diff = mhsa_args->k->diff;       // F x L
    float *v_diff = mhsa_args->v->diff;       // F x L

    float *outDiff = mhsa_args->output->diff; // L x E
    float *inputDiff = mhsa_args->input->diff; // L x E
    float *attention_map_diff = mhsa_args->attention_map->diff; // F x L
    float *softmax_buffer_diff = mhsa_args->softmax_buffer->diff;

    float *coeffDiffWinQ = mhsa_args->coeff_in_q->diff; // E x F
    float *coeffDiffWinK = mhsa_args->coeff_in_k->diff; // E x F
    float *coeffDiffWinV = mhsa_args->coeff_in_v->diff; // E x F
    float *coeffDiffWout = mhsa_args->coeff_out->diff; // E x F

    float scaling = q_rsqrt((float) H);

    // ================================================== BACKPROP 7 ==================================================
    // INITIAL OP [A @ B -> C]
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWout @ attention_map -> outData ~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~     E x F     @     F x L     ->  E x L  ~~~~~~~~~~~~~~~~~~~~~~
    //
    // ----------------------------------------------------------------------------------------------------------------
    // BACKPROP 7.1 [dC @ B^T -> dA]
    // ~~~~~~~~~~~~~~~~~~~~~~ attention_map -T-> temp [attention_map ^ T] ~~~~~~~~~~~~~~~~~~~~~~                (T1)
    // ~~~~~~~~~~~~~~~~~~~~~~     F x L     -T->         L x F            ~~~~~~~~~~~~~~~~~~~~~~
    //
    // ~~~~~~~~~~~~~~~~~~~~~~ outDiff @ temp [attention_map ^ T] -> coeffDiffWout ~~~~~~~~~~~~~~~~~~~~~~        (M1)
    // ~~~~~~~~~~~~~~~~~~~~~~  E x L  @           L x F          ->     E x F    ~~~~~~~~~~~~~~~~~~~~~~
    //
    // BACKPROP 7.2 [A^T @ dC -> dB]
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWout -T-> temp [coeffDataWout ^ T] ~~~~~~~~~~~~~~~~~~~~~~                (T2)
    // ~~~~~~~~~~~~~~~~~~~~~~     E x F     -T->         F x E            ~~~~~~~~~~~~~~~~~~~~~~
    //
    // ~~~~~~~~~~~~~~~~~~~~~~ temp [coeffDataWout ^ T] @ outDiff -> attention_map_diff ~~~~~~~~~~~~~~~~~~~~~~   (M2)
    // ~~~~~~~~~~~~~~~~~~~~~~         F x E            @  E x L  ->       F x L        ~~~~~~~~~~~~~~~~~~~~~~

    // T1
    struct transp_args transp_args1;
    transp_args1.matrix = attention_map;
    transp_args1.transp_matrix = temp;
    transp_args1.N = F;
    transp_args1.M = L;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args1);

    #ifdef DEBUG
    printf("\n\n\n~~~~~~~~~~~~~~~BACKWARD PASS~~~~~~~~~~~~~~~\n\n");
    printf("\nT1 result\n\nattention_map: %d %d\n", F, L);
    for (int j=0; j<H*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  transp_args1.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, F);
    for (int j=0; j<L*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ", transp_args1.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M1
    struct matMul_args matMul_args1;
    matMul_args1.A = outDiff;
    matMul_args1.B = temp;
    matMul_args1.C = coeffDiffWout;
    matMul_args1.N = E;
    matMul_args1.K = L;
    matMul_args1.M = F;
    matMul_args1.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args1);
    #else
    struct mm_manager_args man_args1;
    man_args1.mm_args = &matMul_args1;
    man_args1.layer_type = LAYER_LINEAR;
    man_args1.step_type = STEP_FW;
    man_args1.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args1);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM1 result\n\noutDiff: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  matMul_args1.A[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, F);
    for (int j=0; j<L*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ", matMul_args1.B[j]);
    }
    printf("\n");

    printf("\ncoeffDiffWout: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ", matMul_args1.C[j]);
    }
    printf("\n\n");
    #endif

    // T2
    struct transp_args transp_args2;
    transp_args2.matrix = coeffDataWout;
    transp_args2.transp_matrix = temp;
    transp_args2.N = E;
    transp_args2.M = F;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args2);

    #ifdef DEBUG
    printf("\n\n\ncoeffDataWout: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  transp_args2.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", transp_args2.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M2
    struct matMul_args matMul_args2;
    matMul_args2.A = temp;
    matMul_args2.B = outDiff;
    matMul_args2.C = attention_map_diff;
    matMul_args2.N = F;
    matMul_args2.K = E;
    matMul_args2.M = L;
    matMul_args2.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args2);
    #else
    struct mm_manager_args man_args2;
    man_args2.mm_args = &matMul_args2;
    man_args2.layer_type = LAYER_LINEAR;
    man_args2.step_type = STEP_FW;
    man_args2.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args2);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM2 result\n\ntemp: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ",  matMul_args2.A[j]);
    }
    printf("\n");

    printf("\noutDiff: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args2.B[j]);
    }
    printf("\n");

    printf("\nattention_map_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args2.C[j]);
    }
    printf("\n\n");
    #endif

    // Cycle on the heads
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ F -> H ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    for (int i = 0; i < n_heads; i++) {
        // ================================================ BACKPROP 6 ================================================
        // INITIAL OPS
        // ~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer [softmax_buffer ^ T] -T-> temp [softmax_buffer] ~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~               L x L                 -T->        L x L          ~~~~~~~~~~~~~~~~~~~~~~
        // [A @ B -> C]
        // ~~~~~~~~~~~~~~~~~~~~~~   v   @ temp [softmax_buffer] -> attention_map ~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~ H x L @        L x L          ->     H x L     ~~~~~~~~~~~~~~~~~~~~~~
        //
        // ------------------------------------------------------------------------------------------------------------
        // BACKPROP 6.1 [dC @ B^T -> dA]
        // ~~~~~~~~~~~~~~~~~ attention_map_diff @ softmax_buffer [softmax_buffer ^ T] -> v_diff ~~~~~~~~~~~~~~~~~~  (M3)
        // ~~~~~~~~~~~~~~~~~       H x L        @               L x L                 -> H x L  ~~~~~~~~~~~~~~~~~~
        //
        // BACKPROP 6.2 [A^T @ dC -> dB]
        // ~~~~~~~~~~~~~~~~~~~~~~   v   -T-> temp [v ^ T] ~~~~~~~~~~~~~~~~~~~~~~                                    (T3)
        // ~~~~~~~~~~~~~~~~~~~~~~ H x L -T->   L x H      ~~~~~~~~~~~~~~~~~~~~~~
        //
        // ~~~~~~~~~ temp [v ^ T] @ attention_map_diff -> softmax_buffer_diff [softmax_buffer_diff ^ T] ~~~~~~~~~   (M4)
        // ~~~~~~~~~    L X H     @      H x L         ->                    L x L                      ~~~~~~~~~

        // M3
        struct matMul_args matMul_args3;
        matMul_args3.A = attention_map_diff + i * L * H;
        matMul_args3.B = softmax_buffer + i * L * L;
        matMul_args3.C = v_diff + i * L * H;
        matMul_args3.N = H;
        matMul_args3.K = L;
        matMul_args3.M = L;
        matMul_args3.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args3);
        #else
        struct mm_manager_args man_args3;
        man_args3.mm_args = &matMul_args3;
        man_args3.layer_type = LAYER_LINEAR;
        man_args3.step_type = STEP_FW;
        man_args3.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args3);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M3 result\n\nattention_map_diff: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  matMul_args3.A[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args3.B[j]);
        }
        printf("\n");

        printf("\nv_diff: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args3.C[j]);
        }
        printf("\n\n");
        #endif

        // T3
        struct transp_args transp_args3;
        transp_args3.matrix = v + i * L * H;
        transp_args3.transp_matrix = temp;
        transp_args3.N = H;
        transp_args3.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args3);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T3 result\n\nv: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args3.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", transp_args3.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // M4
        struct matMul_args matMul_args4;
        matMul_args4.A = temp;
        matMul_args4.B = attention_map_diff + i * L * H;
        matMul_args4.C = softmax_buffer_diff + i * L * L;
        matMul_args4.N = L;
        matMul_args4.K = H;
        matMul_args4.M = L;
        matMul_args4.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args4);
        #else
        struct mm_manager_args man_args4;
        man_args4.mm_args = &matMul_args4;
        man_args4.layer_type = LAYER_LINEAR;
        man_args4.step_type = STEP_FW;
        man_args4.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args4);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M4 result\n\ntemp: %d %d\n", i, L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ",  matMul_args4.A[j]);
        }
        printf("\n");

        printf("\nattention_map_diff: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args4.B[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer_diff: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args4.C[j]);
        }
        printf("\n\n");
        #endif


        // ================================================ BACKPROP 5 ================================================
        // INITIAL OP
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer -T-> temp [softmax_buffer ^ T]  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~      L x L     -T->         L x L              ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~ temp [softmax_buffer ^ T] -SM-> softmax_buffer [softmax_buffer ^ T]  ~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~          L x L            -SM->               L x L                  ~~~~~~~~~~~~~~~~~~
        //
        // ------------------------------------------------------------------------------------------------------------
        // BACKPROP
        // ~~~~~~~~~~~~~~~~~~ "IN-PLACE"_TRANSFORM (softmax_buffer_diff) [L x L] ~~~~~~~~~~~~~~~~~~                               (T4 & C1)
        // ~ softmax_buffer [softmax_buffer ^ T] & softmax_buffer_diff [softmax_buffer_diff ^ T] -BW_SM->  grad [sm_diff ^ T] ~   (SM)
        // ~               L x L                 &                  L x L                        -BW_SM->       L x L         ~
        // ~~~~~~~~~~~~~~~~~~ "IN-PLACE"_TRANSFORM (grad) [L x L] ~~~~~~~~~~~~~~~~~~                                              (T5 & C2)

        // T4
        struct transp_args transp_args4;
        transp_args4.matrix = softmax_buffer_diff + i * L * L;
        transp_args4.transp_matrix = temp;
        transp_args4.N = L;
        transp_args4.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args4);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T4 result\n\nsoftmax_buffer: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args4.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", transp_args4.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // C1
        struct copy_args copy_args1;
        copy_args1.from = temp;
        copy_args1.to = softmax_buffer_diff + i * L * L;
        copy_args1.size = L * L;

        pi_cl_team_fork(NUM_CORES, copy, &copy_args1);

        #ifdef DEBUG
        printf("\n\n\nHead %d - C1 result\n\ntemp: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  copy_args1.from[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer_diff: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", copy_args1.to[j]);
        }
        printf("\n\n");
        #endif

        // SM
        struct softmax_args softmax_arg;
        softmax_arg.input_diff = grad;
        softmax_arg.output_data = softmax_buffer + i * L * L;
        softmax_arg.output_diff = softmax_buffer_diff + i * L * L;
        softmax_arg.sums = sums;
        softmax_arg.H = L;
        softmax_arg.W = L;

        pulp_softmax_fp32_bw_cl(&softmax_arg);

        #ifdef DEBUG
        printf("\n\n\nHead %d - softmax backprop result\n\nsoftmax_buffer: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  softmax_arg.output_data[j]);
        }
        printf("\n");

        printf("\nsoftmax_buffer_diff: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", softmax_arg.output_diff[j]);
        }
        printf("\n");

        printf("\ngrad: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", softmax_arg.input_diff[j]);
        }
        printf("\n\n");
        #endif

        // T5
        struct transp_args transp_args5;
        transp_args5.matrix = grad;
        transp_args5.transp_matrix = temp;
        transp_args5.N = L;
        transp_args5.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args5);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T5 result\n\ngrad: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args5.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", transp_args5.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // C2
        struct copy_args copy_args2;
        copy_args2.from = temp;
        copy_args2.to = grad;
        copy_args2.size = L * L;

        pi_cl_team_fork(NUM_CORES, copy, &copy_args2);

        #ifdef DEBUG
        printf("\n\n\nHead %d - C2 result\n\ntemp: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  copy_args2.from[j]);
        }
        printf("\n");

        printf("\ngrad: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", copy_args1.to[j]);
        }
        printf("\n\n");
        #endif


        // ================================================ BACKPROP 4 ================================================
        // INITIAL OP
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ softmax_buffer *= scalar ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~           L x L          ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        //
        // ------------------------------------------------------------------------------------------------------------
        // BACKPROP
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ grad [softmax_buffer] *= scalar ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~               L x L             ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        struct scalar_mul_args s_m_args;
        s_m_args.input = grad;
        s_m_args.scalar = scaling;
        s_m_args.dim = L * L;

        #ifdef DEBUG
        printf("\n\n\nHead %d - backprop scalar result\n\ngrad (BEFORE scaling): %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", s_m_args.input[j]);
        }
        printf("\n\n");
        #endif

        pi_cl_team_fork(NUM_CORES, pulp_scalar_mul_fp32_cl, &s_m_args);

        #ifdef DEBUG
        printf("\n\n\ngrad (AFTER scaling): %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", s_m_args.input[j]);
        }
        printf("\n\n");
        #endif


        // ================================================ BACKPROP 3 ================================================
        // INITIAL OP
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   k   -T-> temp [k ^ T] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ H x L  ->     L x H     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // [A @ B -> C]
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ temp [k ^ T] @   q   -> softmax_buffer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    L x H     @ H x L ->     L x L      ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        //
        // ------------------------------------------------------------------------------------------------------------
        // BACKPROP 3.1 [dC @ B^T -> dA]
        // ~~~~~~~~~~~~~~~~~~~~~~   q   -T-> temp [q ^ T] ~~~~~~~~~~~~~~~~~~~~~~                                     (T6)
        // ~~~~~~~~~~~~~~~~~~~~~~ H x L -T->    L x H     ~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  grad @ temp [q ^ T] -> k_diff [k_diff ^ T] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ (M5)
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ L x L @     L x H    ->      L x H          ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // ~~~~~~~~~~~~~~~~~~ "IN-PLACE"_TRANSFORM (k_diff) [L x H] -> [H x L] ~~~~~~~~~~~~~~~~~~                    (T7 & C3)
        //
        // BACKPROP 3.2 [A^T @ dC -> dB]
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~   k   @ grad  -> q_diff ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~                     (M6)
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ H x L @ L x L -> H x L  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        // T6
        struct transp_args transp_args6;
        transp_args6.matrix = q + i * L * H;
        transp_args6.transp_matrix = temp;
        transp_args6.N = H;
        transp_args6.M = L;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args6);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T6 result\n\nq: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  transp_args6.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", transp_args6.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // M5
        struct matMul_args matMul_args5;
        matMul_args5.A = grad;
        matMul_args5.B = temp;
        matMul_args5.C = k_diff + i * L * H;
        matMul_args5.N = L;
        matMul_args5.K = L;
        matMul_args5.M = H;
        matMul_args5.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args5);
        #else
        struct mm_manager_args man_args5;
        man_args5.mm_args = &matMul_args5;
        man_args5.layer_type = LAYER_LINEAR;
        man_args5.step_type = STEP_FW;
        man_args5.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args5);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M5 result\n\ngrad: %d %d\n", i, L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  matMul_args5.A[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", matMul_args5.B[j]);
        }
        printf("\n");

        printf("\nk_diff: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", matMul_args5.C[j]);
        }
        printf("\n\n");
        #endif

        // T7
        struct transp_args transp_args7;
        transp_args7.matrix = k_diff + i * L * H;
        transp_args7.transp_matrix = temp;
        transp_args7.N = L;
        transp_args7.M = H;

        pi_cl_team_fork(NUM_CORES, transpose, &transp_args7);

        #ifdef DEBUG
        printf("\n\n\nHead %d - T7 result\n\nk_diff: %d %d\n", i, L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ",  transp_args7.matrix[j]);
        }
        printf("\n");

        printf("\ntemp: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", transp_args7.transp_matrix[j]);
        }
        printf("\n\n");
        #endif

        // C3
        struct copy_args copy_args3;
        copy_args3.from = temp;
        copy_args3.to = k_diff + i * L * H;
        copy_args3.size = L * H;

        pi_cl_team_fork(NUM_CORES, copy, &copy_args3);

        #ifdef DEBUG
        printf("\n\n\nHead %d - C3 result\n\ntemp: %d %d\n", i, L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ",  copy_args3.from[j]);
        }
        printf("\n");

        printf("\nk_diff: %d %d\n", L, H);
        for (int j=0; j<L*H; j++){
            if(!(j%(H))) printf("\n");
            printf("%.8f ", copy_args2.to[j]);
        }
        printf("\n\n");
        #endif

        // M6
        struct matMul_args matMul_args6;
        matMul_args6.A = k + i * L * H;
        matMul_args6.B = grad;
        matMul_args6.C = q_diff + i * L * H;
        matMul_args6.N = H;
        matMul_args6.K = L;
        matMul_args6.M = L;
        matMul_args6.trans_B = 0;

        #ifndef OPTIMIZE
        pi_cl_team_fork(NUM_CORES, mm, &matMul_args6);
        #else
        struct mm_manager_args man_args6;
        man_args6.mm_args = &matMul_args6;
        man_args6.layer_type = LAYER_LINEAR;
        man_args6.step_type = STEP_FW;
        man_args6.matmul_type = opt_matmul_type; //MATMUL_TYPE
        pi_cl_team_fork(NUM_CORES, mm_manager, &man_args6);
        #endif

        #ifdef DEBUG
        printf("\n\n\nHead %d - M6 result\n\nk: %d %d\n", i, H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ",  matMul_args6.A[j]);
        }
        printf("\n");

        printf("\ngrad: %d %d\n", L, L);
        for (int j=0; j<L*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args6.B[j]);
        }
        printf("\n");

        printf("\nq_diff: %d %d\n", H, L);
        for (int j=0; j<H*L; j++){
            if(!(j%(L))) printf("\n");
            printf("%.8f ", matMul_args6.C[j]);
        }
        printf("\n\n");
        #endif
    }
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ H -> F ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    // ================================================== BACKPROP 1 ==================================================
    // INITIAL OP [A @ B -> C]
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinQ @ inputData ->   q   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~     F x E     @  E x L    -> F x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinK @ inputData ->   k   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~     F x E     @  E x L    -> F x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinV @ inputData ->   v   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~     F x E     @  E x L    -> F x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //
    // ------------------------------------------------------------------------------------------------------------
    // BACKPROP 1.1 [dC @ B^T -> dA]
    // ~~~~~~~~~~~~~~~~~~~~~~ inputData -T-> temp [inputData ^ T] ~~~~~~~~~~~~~~~~~~~~~~                         (T8)
    // ~~~~~~~~~~~~~~~~~~~~~~   E x L   -T->        L x E         ~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~ q_diff  @ temp [inputData ^ T] -> coeffDiffWinQ ~~~~~~~~~~~~~~~~~~~~~~~~~~~   (M7_q)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~  F x L  @       L x E          ->     F x E     ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~ k_diff  @ temp [inputData ^ T] -> coeffDiffWinK ~~~~~~~~~~~~~~~~~~~~~~~~~~~   (M7_k)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~  F x L  @       L x E          ->     F x E     ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~ v_diff  @ temp [inputData ^ T] -> coeffDiffWinV ~~~~~~~~~~~~~~~~~~~~~~~~~~~   (M7_v)
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~  F x L  @       L x E          ->     F x E     ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //
    // BACKPROP 1.2 [A^T @ dC -> dB]
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinQ [coeffDataWinQ ^ T] @ q_diff -> temp  ~~~~~~~~~~~~~~~~~~~~~~~~~~~    (M8_q)
    // ~~~~~~~~~~~~~~~~~~~~~~              E x F                @ F x L  -> E x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~ inputDiff + temp  -> inputDiff  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    (SUM_q)
    // ~~~~~~~~~~~~~~~~~~~~~~   E x L   + E x L ->   E x L    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinK [coeffDataWinK ^ T] @ k_diff -> temp  ~~~~~~~~~~~~~~~~~~~~~~~~~~~    (M8_k)
    // ~~~~~~~~~~~~~~~~~~~~~~              E x F                @ F x L  -> E x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~ inputDiff + temp  -> inputDiff  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    (SUM_k)
    // ~~~~~~~~~~~~~~~~~~~~~~   E x L   + E x L ->   E x L    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    //
    // ~~~~~~~~~~~~~~~~~~~~~~ coeffDataWinV [coeffDataWinV ^ T] @ v_diff -> temp  ~~~~~~~~~~~~~~~~~~~~~~~~~~~    (M8_v)
    // ~~~~~~~~~~~~~~~~~~~~~~              E x F                @ F x L  -> E x L ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // ~~~~~~~~~~~~~~~~~~~~~~ inputDiff + temp  -> inputDiff  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    (SUM_v)
    // ~~~~~~~~~~~~~~~~~~~~~~   E x L   + E x L ->   E x L    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    // T8
    struct transp_args transp_args8;
    transp_args8.matrix = inputData;
    transp_args8.transp_matrix = temp;
    transp_args8.N = E;
    transp_args8.M = L;

    pi_cl_team_fork(NUM_CORES, transpose, &transp_args8);

    #ifdef DEBUG
    printf("\n\n\nT8 result\n\ninputData: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  transp_args8.matrix[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, E);
    for (int j=0; j<L*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", transp_args8.transp_matrix[j]);
    }
    printf("\n\n");
    #endif

    // M7_q
    struct matMul_args matMul_args7_q;
    matMul_args7_q.A = q_diff;
    matMul_args7_q.B = temp;
    matMul_args7_q.C = coeffDiffWinQ;
    matMul_args7_q.N = F;
    matMul_args7_q.K = L;
    matMul_args7_q.M = E;
    matMul_args7_q.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args7_q);
    #else
    struct mm_manager_args man_args7_q;
    man_args7_q.mm_args = &matMul_args7_q;
    man_args7_q.layer_type = LAYER_LINEAR;
    man_args7_q.step_type = STEP_FW;
    man_args7_q.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args7_q);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM7_q result\n\nq_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  matMul_args7_q.A[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, E);
    for (int j=0; j<L*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_q.B[j]);
    }
    printf("\n");

    printf("\ncoeffDiffWinQ: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_q.C[j]);
    }
    printf("\n\n");
    #endif

    // M7_k
    struct matMul_args matMul_args7_k;
    matMul_args7_k.A = k_diff;
    matMul_args7_k.B = temp;
    matMul_args7_k.C = coeffDiffWinK;
    matMul_args7_k.N = F;
    matMul_args7_k.K = L;
    matMul_args7_k.M = E;
    matMul_args7_k.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args7_k);
    #else
    struct mm_manager_args man_args7_k;
    man_args7_k.mm_args = &matMul_args7_k;
    man_args7_k.layer_type = LAYER_LINEAR;
    man_args7_k.step_type = STEP_FW;
    man_args7_k.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args7_k);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM7_k result\n\nk_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  matMul_args7_k.A[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, E);
    for (int j=0; j<L*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_k.B[j]);
    }
    printf("\n");

    printf("\ncoeffDiffWinK: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_k.C[j]);
    }
    printf("\n\n");
    #endif

    // M7_v
    struct matMul_args matMul_args7_v;
    matMul_args7_v.A = v_diff;
    matMul_args7_v.B = temp;
    matMul_args7_v.C = coeffDiffWinV;
    matMul_args7_v.N = F;
    matMul_args7_v.K = L;
    matMul_args7_v.M = E;
    matMul_args7_v.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args7_v);
    #else
    struct mm_manager_args man_args7_v;
    man_args7_v.mm_args = &matMul_args7_v;
    man_args7_v.layer_type = LAYER_LINEAR;
    man_args7_v.step_type = STEP_FW;
    man_args7_v.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args7_v);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM7_v result\n\nv_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ",  matMul_args7_v.A[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", L, E);
    for (int j=0; j<L*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_v.B[j]);
    }
    printf("\n");

    printf("\ncoeffDiffWinV: %d %d\n", F, E);
    for (int j=0; j<F*E; j++){
        if(!(j%(E))) printf("\n");
        printf("%.8f ", matMul_args7_v.C[j]);
    }
    printf("\n\n");
    #endif

    // M8_q
    struct matMul_args matMul_args8_q;
    matMul_args8_q.A = coeffDataWinQ;
    matMul_args8_q.B = q_diff;
    matMul_args8_q.C = temp;
    matMul_args8_q.N = E;
    matMul_args8_q.K = F;
    matMul_args8_q.M = L;
    matMul_args8_q.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args8_q);
    #else
    struct mm_manager_args man_args8_q;
    man_args8_q.mm_args = &matMul_args8_q;
    man_args8_q.layer_type = LAYER_LINEAR;
    man_args8_q.step_type = STEP_FW;
    man_args8_q.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args8_q);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM8_q result\n\ncoeffDataWinQ [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  matMul_args8_q.A[j]);
    }
    printf("\n");

    printf("\nq_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_q.B[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_q.C[j]);
    }
    printf("\n\n");
    #endif

    // SUM_q
    struct vect_sum_args vect_sum_args;
    vect_sum_args.op_1 = inputDiff;
    vect_sum_args.op_2 = temp;
    vect_sum_args.dest = inputDiff;
    vect_sum_args.size = E * L;

    pi_cl_team_fork(NUM_CORES, vect_sum, &vect_sum_args);

    // M8_k
    struct matMul_args matMul_args8_k;
    matMul_args8_k.A = coeffDataWinK;
    matMul_args8_k.B = k_diff;
    matMul_args8_k.C = temp;
    matMul_args8_k.N = E;
    matMul_args8_k.K = F;
    matMul_args8_k.M = L;
    matMul_args8_k.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args8_k);
    #else
    struct mm_manager_args man_args8_k;
    man_args8_k.mm_args = &matMul_args8_k;
    man_args8_k.layer_type = LAYER_LINEAR;
    man_args8_k.step_type = STEP_FW;
    man_args8_k.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args8_k);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM8_k result\n\ncoeffDataWinK [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  matMul_args8_k.A[j]);
    }
    printf("\n");

    printf("\nk_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_k.B[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_k.C[j]);
    }
    printf("\n\n");
    #endif

    // SUM_k
    pi_cl_team_fork(NUM_CORES, vect_sum, &vect_sum_args);

    // M8_v
    struct matMul_args matMul_args8_v;
    matMul_args8_v.A = coeffDataWinV;
    matMul_args8_v.B = v_diff;
    matMul_args8_v.C = temp;
    matMul_args8_v.N = E;
    matMul_args8_v.K = F;
    matMul_args8_v.M = L;
    matMul_args8_v.trans_B = 0;

    #ifndef OPTIMIZE
    pi_cl_team_fork(NUM_CORES, mm, &matMul_args8_v);
    #else
    struct mm_manager_args man_args8_v;
    man_args8_v.mm_args = &matMul_args8_v;
    man_args8_v.layer_type = LAYER_LINEAR;
    man_args8_v.step_type = STEP_FW;
    man_args8_v.matmul_type = opt_matmul_type; //MATMUL_TYPE
    pi_cl_team_fork(NUM_CORES, mm_manager, &man_args8_v);
    #endif

    #ifdef DEBUG
    printf("\n\n\nM8_v result\n\ncoeffDataWinV [^T]: %d %d\n", E, F);
    for (int j=0; j<E*F; j++){
        if(!(j%(F))) printf("\n");
        printf("%.8f ",  matMul_args8_v.A[j]);
    }
    printf("\n");

    printf("\nv_diff: %d %d\n", F, L);
    for (int j=0; j<F*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_v.B[j]);
    }
    printf("\n");

    printf("\ntemp: %d %d\n", E, L);
    for (int j=0; j<E*L; j++){
        if(!(j%(L))) printf("\n");
        printf("%.8f ", matMul_args8_v.C[j]);
    }
    printf("\n\n");
    #endif

    // SUM_v
    pi_cl_team_fork(NUM_CORES, vect_sum, &vect_sum_args);
}
