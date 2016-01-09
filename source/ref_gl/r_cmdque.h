/*
Copyright (C) 2016 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef R_CMDQUEUE_H
#define R_CMDQUEUE_H

#include "r_local.h"

// public frontend -> frontend/backend commands

// frame commands
// a valid frame should begin and end with REF_CMD_BEGIN_FRAME and REF_CMD_END_FRAME cmds

enum
{
    REF_CMD_BEGIN_FRAME,
    REF_CMD_END_FRAME,
    
    REF_CMD_DRAW_STRETCH_PIC,
    REF_CMD_DRAW_STRETCH_POLY,
    
    REF_CMD_CLEAR_SCENE,
    REF_CMD_ADD_ENTITY_TO_SCENE,
    REF_CMD_ADD_LIGHT_TO_SCENE,
    REF_CMD_ADD_POLY_TO_SCENE,
    REF_CMD_ADD_LIGHT_STYLE_TO_SCENE,
    REF_CMD_RENDER_SCENE,
    
    REF_CMD_SET_SCISSOR,
    REF_CMD_RESET_SCISSOR,
    
    REF_CMD_SET_CUSTOM_COLOR,
    
	REF_CMD_SYNC,

	REF_CMD_DRAW_STRETCH_RAW,
	REF_CMD_DRAW_STRETCH_RAW_YUV,

    NUM_REF_CMDS
};

typedef struct
{
	uint8_t			buf[0x400000];
	uint32_t		frameId;
	size_t			len;
} ref_cmdbuf_t;

typedef unsigned (*refCmdHandler_t)( const void * );
extern refCmdHandler_t refCmdHandlers[];

void RF_IssueBeginFrameCmd( ref_cmdbuf_t *frame, float cameraSeparation, bool forceClear, bool forceVsync );
void RF_IssueEndFrameCmd( ref_cmdbuf_t *frame );
void RF_IssueDrawRotatedStretchPicCmd( ref_cmdbuf_t *frame, int x, int y, int w, int h,
	float s1, float t1, float s2, float t2, float angle, const vec4_t color, const shader_t *shader );
void RF_IssueDrawStretchPolyCmd( ref_cmdbuf_t *frame, const poly_t *poly, float x_offset, float y_offset );
void RF_IssueClearSceneCmd( ref_cmdbuf_t *frame );
void RF_IssueAddEntityToSceneCmd( ref_cmdbuf_t *frame, const entity_t *ent );
void RF_IssueAddLightToSceneCmd( ref_cmdbuf_t *frame, const vec3_t org, float intensity, float r, float g, float b );
void RF_IssueAddPolyToSceneCmd( ref_cmdbuf_t *frame, const poly_t *poly );
void RF_IssueAddLightStyleToSceneCmd( ref_cmdbuf_t *frame, int style, float r, float g, float b );
void RF_IssueRenderSceneCmd( ref_cmdbuf_t *frame, const refdef_t *fd );
void RF_IssueSetScissorCmd( ref_cmdbuf_t *frame, int x, int y, int w, int h );
void RF_IssueResetScissorCmd( ref_cmdbuf_t *frame );
void RF_IssueSetCustomColorCmd( ref_cmdbuf_t *frame, int num, int r, int g, int b );
void RF_IssueSyncCmd( ref_cmdbuf_t *frame );
void RF_IssueDrawStretchRawCmd( ref_cmdbuf_t *frame, int x, int y, int w, int h, float s1, float t1, float s2, float t2 );
void RF_IssueDrawStretchRawYUVCmd( ref_cmdbuf_t *frame, int x, int y, int w, int h, float s1, float t1, float s2, float t2 );

// ==========

// inter-frame thread-safe pipe for commands
// we need it to process commands that may not be dropped along with respective frames

enum
{
	REF_PIPE_CMD_INIT,
	REF_PIPE_CMD_SHUTDOWN,
	REF_PIPE_CMD_SURFACE_CHANGE,
    REF_PIPE_CMD_SCREEN_SHOT,
    REF_PIPE_CMD_ENV_SHOT,

    NUM_REF_PIPE_CMDS
};

typedef unsigned (*refPipeCmdHandler_t)( const void * );
extern refPipeCmdHandler_t refPipeCmdHandlers[];

void RF_IssueInitReliableCmd( qbufPipe_t *pipe );
void RF_IssueShutdownReliableCmd( qbufPipe_t *pipe );
void RF_IssueSurfaceChangeReliableCmd( qbufPipe_t *pipe );
void RF_IssueScreenShotReliableCmd( qbufPipe_t *pipe, const char *path, const char *name, bool silent );
void RF_IssueEnvShotReliableCmd( qbufPipe_t *pipe, const char *path, const char *name, unsigned pixels );
void RF_IssueAviShotReliableCmd( qbufPipe_t *pipe, const char *path, const char *name, int x, int y, int w, int h );

#endif // R_CMDQUEUE_H
