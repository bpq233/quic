#ifndef _LOSS_FILTER_H_INCLUDED_
#define _LOSS_FILTER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <stdbool.h>

typedef struct {
    float loss[100];
    float loss_now;
    uint64_t rank;
    uint64_t cnt;   

}Loss_Filter;

void init_Loss_Filter(Loss_Filter *loss_filter);
void insertLoss(Loss_Filter *loss_filter, float loss);
void updateRank(Loss_Filter *loss_filter, float loss);

#endif
