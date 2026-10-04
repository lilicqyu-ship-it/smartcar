#ifndef MW_DIAG_SERVICE_H
#define MW_DIAG_SERVICE_H
#include "Ifx_Types.h"
void DIAG_init(void);
void DIAG_tick(void); /* CPU0 control task only */
boolean DIAG_command(const uint8 *bytes,uint8 len);
void DIAG_onCommand(uint8 cmd,const uint8 *data,uint8 len);
#endif
