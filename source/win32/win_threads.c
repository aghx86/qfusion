/*
Copyright (C) 2013 Victor Luchits

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

#include "../qcommon/qcommon.h"
#include "../qcommon/sys_threads.h"
#include "winquake.h"

#define QF_USE_CRITICAL_SECTIONS

struct qthread_s {
	HANDLE h;
};

struct qcondvar_s {
	CONDITION_VARIABLE c;
};

#ifdef QF_USE_CRITICAL_SECTIONS
struct qmutex_s {
	CRITICAL_SECTION h;
};

/*
* Sys_Mutex_Create
*/
int Sys_Mutex_Create( qmutex_t **pmutex )
{
	qmutex_t *mutex;

	mutex = ( qmutex_t * )Q_malloc( sizeof( *mutex ) );
	if( !mutex ) {
		return -1;
	}
	InitializeCriticalSection( &mutex->h );

	*pmutex = mutex;
	return 0;
}

/*
* Sys_Mutex_Destroy
*/
void Sys_Mutex_Destroy( qmutex_t *mutex )
{
	if( !mutex ) {
		return;
	}
	DeleteCriticalSection( &mutex->h );
	Q_free( mutex );
}

/*
* Sys_Mutex_Lock
*/
void Sys_Mutex_Lock( qmutex_t *mutex )
{
	EnterCriticalSection( &mutex->h );
}

/*
* Sys_Mutex_Unlock
*/
void Sys_Mutex_Unlock( qmutex_t *mutex )
{
	LeaveCriticalSection( &mutex->h );
}
#else
struct qmutex_s {
	HANDLE h;
};

/*
* Sys_Mutex_Create
*/
int Sys_Mutex_Create( qmutex_t **pmutex )
{
	qmutex_t *mutex;

	HANDLE h = CreateMutex( NULL, FALSE, NULL );
	if( h == NULL ) {
		return GetLastError();
	}
	
	mutex = ( qmutex_t * )Q_malloc( sizeof( *mutex ) );
	if( !mutex ) {
		return -1;
	}
	mutex->h = h;
	*pmutex = mutex;
	return 0;
}

/*
* Sys_Mutex_Destroy
*/
void Sys_Mutex_Destroy( qmutex_t *mutex )
{
	if( !mutex ) {
		return;
	}
	CloseHandle( mutex->h );
	Q_free( mutex );
}

/*
* Sys_Mutex_Lock
*/
void Sys_Mutex_Lock( qmutex_t *mutex )
{
	WaitForSingleObject( mutex->h, INFINITE );
}

/*
* Sys_Mutex_Unlock
*/
void Sys_Mutex_Unlock( qmutex_t *mutex )
{
	ReleaseMutex( mutex->h );
}
#endif

/*
* Sys_Thread_Create
*/
int Sys_Thread_Create( qthread_t **pthread, void *(*routine) (void*), void *param )
{
	qthread_t *thread;

	HANDLE h = CreateThread(
		NULL,
		0,
		(LPTHREAD_START_ROUTINE) routine,
		(LPVOID) param,
		0,
        NULL
	);

	if( h == NULL ) {
		return GetLastError();
	}

	thread = ( qthread_t * )Q_malloc( sizeof( *thread ) );
	thread->h = h;
	*pthread = thread;
	return 0;
}

/*
* Sys_Thread_Join
*/
void Sys_Thread_Join( qthread_t *thread )
{
	if( thread ) {
		WaitForSingleObject( thread->h, INFINITE );
		CloseHandle( thread->h );
	}
}
/*
* Sys_Thread_Cancel
*/
int Sys_Thread_Cancel( qthread_t *thread )
{
	if( thread ) {
		return TerminateThread( thread->h, 0 ) ? 0 : GetLastError();
	}
    return 1;
}

/*
* Sys_Thread_Yield
*/
void Sys_Thread_Yield( void )
{
	Sys_Sleep( 0 );
}

/*
* Sys_Atomic_Add
*/
int Sys_Atomic_Add( volatile int *value, int add, qmutex_t *mutex )
{
	return InterlockedExchangeAdd( (volatile LONG*)value, add );
}

/*
* Sys_CondVar_Create
*/
int Sys_CondVar_Create( qcondvar_t **pcond )
{
	qcondvar_t *cond;

	cond = ( qcondvar_t * )Q_malloc( sizeof( *cond ) );
	if( !pcond ) {
		return -1;
	}
	InitializeConditionVariable( &cond->c );

	*pcond = cond;
	return 0;
}

/*
* Sys_CondVar_Destroy
*/
void Sys_CondVar_Destroy( qcondvar_t *cond )
{
	Q_free( cond );
}

/*
* Sys_CondVar_Wait
*/
bool Sys_CondVar_Wait( qcondvar_t *cond, qmutex_t *mutex, unsigned int timeout_msec )
{
	if( !cond || !mutex ) {
		return false;
	}
#ifdef QF_USE_CRITICAL_SECTIONS
	return SleepConditionVariableCS( &cond->c, &mutex->h, timeout_msec ) != 0;
#else
	return false;
#endif
}

/*
* Sys_CondVar_Wake
*/
void Sys_CondVar_Wake( qcondvar_t *cond )
{
	if( !cond ) {
		return;
	}
	WakeConditionVariable( &cond->c );
}
