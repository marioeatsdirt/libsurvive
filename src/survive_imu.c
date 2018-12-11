#include "survive_imu.h"
#include "linmath.h"
#include "math.h"
#include "survive_imu.h"
#include "survive_internal.h"
#include "survive_kalman.h"
#include <assert.h>
#include <memory.h>
#include <minimal_opencv.h>

//#define SV_VERBOSE(...) SV_INFO(__VA_ARGS__)
#define SV_VERBOSE(...)

// Mahoney is due to https://hal.archives-ouvertes.fr/hal-00488376/document
// See also http://www.olliw.eu/2013/imu-data-fusing/#chapter41 and
// http://x-io.co.uk/open-source-imu-and-ahrs-algorithms/
static void mahony_ahrs(SurviveIMUTracker *tracker, LinmathQuat q, const LinmathVec3d _gyro,
						const LinmathVec3d _accel) {
	LinmathVec3d gyro;
	memcpy(gyro, _gyro, 3 * sizeof(FLT));

	LinmathVec3d accel;
	memcpy(accel, _accel, 3 * sizeof(FLT));

	const FLT sample_f = tracker->so->imu_freq;
	const FLT prop_gain = .5;
	const FLT int_gain = 0;

	FLT mag_accel = magnitude3d(accel);

	if (mag_accel != 0.0) {
		scale3d(accel, accel, 1. / mag_accel);

		// Equiv of q^-1 * G
		LinmathVec3d v = {q[1] * q[3] - q[0] * q[2], q[0] * q[1] + q[2] * q[3], q[0] * q[0] - 0.5 + q[3] * q[3]};

		LinmathVec3d error;
		cross3d(error, accel, v);

		if (int_gain > 0.0f) {
			LinmathVec3d fb_correction;
			scale3d(fb_correction, error, int_gain * 2. / sample_f);
			add3d(tracker->integralFB, tracker->integralFB, fb_correction);
			add3d(gyro, gyro, tracker->integralFB);
		}

		scale3d(error, error, prop_gain * 2.);
		add3d(gyro, gyro, error);
	}

	scale3d(gyro, gyro, 0.5 / sample_f);

	LinmathQuat correction = {
		(-q[1] * gyro[0] - q[2] * gyro[1] - q[3] * gyro[2]), (+q[0] * gyro[0] + q[2] * gyro[2] - q[3] * gyro[1]),
		(+q[0] * gyro[1] - q[1] * gyro[2] + q[3] * gyro[0]), (+q[0] * gyro[2] + q[1] * gyro[1] - q[2] * gyro[0])};

	quatadd(q, q, correction);
	quatnormalize(q, q);
}

static const int imu_calibration_iterations = 100;

static void RotateAccel(LinmathVec3d rAcc, const LinmathQuat rot, const LinmathVec3d accel) {
	quatrotatevector(rAcc, rot, accel);
	LinmathVec3d G = {0, 0, -1};
	SurviveContext *ctx = 0;
	SV_VERBOSE("RotateAccel: %f\t" Point3_format, magnitude3d(rAcc), LINMATH_VEC3_EXPAND(rAcc));
	add3d(rAcc, rAcc, G);
	scale3d(rAcc, rAcc, 9.8066);
}

void survive_imu_tracker_integrate_imu(SurviveIMUTracker *tracker, PoserDataIMU *data) {
	SurviveContext *ctx = tracker->so->ctx;
	if (tracker->last_data.datamask == 0) {
		tracker->last_data = *data;
		tracker->imu_kalman_update = data->timecode;
		tracker->obs_kalman_update = data->timecode;
		return;
	}

	// double n = 1. / norm3d(data->accel);
	// if(n > .999 && n < 1.001)
	// tracker->acc_bias = tracker->acc_bias * .95 + (1 / norm3d(data->accel)) * .05;

	// SV_INFO("%7f %7f", n, tracker->acc_bias);

	FLT time_diff =
		survive_timecode_difference(data->timecode, tracker->imu_kalman_update) / (FLT)tracker->so->timebase_hz;
	// printf("i%u %f\n", data->timecode, time_diff);
	LinmathAxisAngleMag aa_rot;
	survive_kalman_get_state(0, &tracker->rot, 0, aa_rot);
	LinmathQuat rot;
	quatfromaxisanglemag(rot, aa_rot);
	assert(time_diff > 0);
	if (time_diff > 1.0) {
		SV_WARN("%s is probably dropping IMU packets; %f time reported between", tracker->so->codename, time_diff);
		assert(time_diff < 10);
	}

	if (tracker->mahony_variance >= 0) {
		LinmathQuat pose_rot;
		quatcopy(pose_rot, rot);
		mahony_ahrs(tracker, pose_rot, data->gyro, data->accel);

		const FLT Hr[] = {1, 0};
		LinmathAxisAngleMag input;
		quattoaxisanglemag(input, pose_rot);

		survive_kalman_predict_update_state(time_diff, &tracker->rot, input, Hr,
											tracker->rot.info.P[0] + tracker->mahony_variance);
		time_diff = 0;
	}

	FLT Rv[2] = {tracker->rot.info.P[0] + tracker->acc_var, tracker->rot.info.P[0] + tracker->gyro_var};

	LinmathVec3d rAcc = {0};
	RotateAccel(rAcc, rot, data->accel);

	const FLT Hp[] = {0, 0, 1};
	survive_kalman_predict_update_state(time_diff, &tracker->position, rAcc, Hp, Rv[0]);

	const FLT Hr[] = {0, 1};
	LinmathAxisAngleMag rot_vel;
	quatrotatevector(rot_vel, rot, data->gyro);
	survive_kalman_predict_update_state(time_diff, &tracker->rot, rot_vel, Hr, Rv[1]);

	tracker->imu_kalman_update = tracker->obs_kalman_update = data->timecode;
}

void survive_imu_tracker_predict(const SurviveIMUTracker *tracker, survive_timecode timecode, SurvivePose *out) {
	if (tracker->position.info.P[0] > 100 || tracker->rot.info.P[0] > 100)
		return;

	FLT t = survive_timecode_difference(timecode, tracker->obs_kalman_update) / (FLT)tracker->so->timebase_hz;

	survive_kalman_get_state(t, &tracker->position, 0, out->Pos);

	LinmathAxisAngleMag r;
	survive_kalman_get_state(t, &tracker->rot, 0, r);
	quatfromaxisanglemag(out->Rot, r);
}

SURVIVE_EXPORT void survive_imu_tracker_update(SurviveIMUTracker *tracker, survive_timecode timecode,
											   SurvivePose *out) {
	survive_imu_tracker_predict(tracker, timecode, out);
}

void survive_imu_tracker_integrate_observation(uint32_t timecode, SurviveIMUTracker *tracker, const SurvivePose *pose,
											   const FLT *R) {
	if (tracker->last_data.datamask == 0) {
		tracker->last_data.datamask = 1;
		tracker->imu_kalman_update = timecode;
		tracker->obs_kalman_update = timecode;
	}

	FLT time_diff = survive_timecode_difference(timecode, tracker->obs_kalman_update) / (FLT)tracker->so->timebase_hz;
	assert(time_diff >= 0 && time_diff < 10);
	// printf("o%u %f\n", timecode, time_diff);
	FLT H[] = {1., time_diff, time_diff * time_diff / 2.};

	survive_kalman_predict_update_state(time_diff, &tracker->position, pose->Pos, H, R[0]);

	LinmathAxisAngleMag aa_rot;
	quattoaxisanglemag(aa_rot, pose->Rot);
	survive_kalman_predict_update_state(time_diff, &tracker->rot, aa_rot, H, R[1]);

	tracker->imu_kalman_update = tracker->obs_kalman_update = timecode;
}

STATIC_CONFIG_ITEM(POSE_POSITION_VARIANCE_SEC, "filter-pose-var-per-sec", 'f', "Position variance per second", 0.001);
STATIC_CONFIG_ITEM(POSE_ROT_VARIANCE_SEC, "filter-pose-rot-var-per-sec", 'f', "Position rotational variance per second",
				   0.01);

STATIC_CONFIG_ITEM(VELOCITY_POSITION_VARIANCE_SEC, "filter-vel-var-per-sec", 'f', "Velocity variance per second", 1.0);
STATIC_CONFIG_ITEM(VELOCITY_ROT_VARIANCE_SEC, "filter-vel-rot-var-per-sec", 'f',
				   "Velocity rotational variance per second", 0.1);

STATIC_CONFIG_ITEM(IMU_ACC_VARIANCE, "imu-acc-variance", 'f', "Variance of accelerometer", 1.0);
STATIC_CONFIG_ITEM(IMU_GYRO_VARIANCE, "imu-gyro-variance", 'f', "Variance of gyroscope", 0.001);
STATIC_CONFIG_ITEM(IMU_MAHONY_VARIANCE, "imu-mahony-variance", 'f', "Variance of mahony filter (negative to disable)",
				   -1.);

STATIC_CONFIG_ITEM(USE_OBS_VELOCITY, "use-obs-velocity", 'i', "Incorporate observed velocity into filter", 1);
STATIC_CONFIG_ITEM(OBS_VELOCITY_POSITION_VAR, "obs-velocity-var", 'f', "Incorporate observed velocity into filter", 1.);
STATIC_CONFIG_ITEM(OBS_VELOCITY_ROTATION_VAR, "obs-velocity-rot-var", 'f', "Incorporate observed velocity into filter",
				   0.001);

void rot_f(FLT t, FLT *F) {
	FLT f[] = {1, t, 0, 1};

	memcpy(F, f, sizeof(FLT) * 4);
}

void pos_f(FLT t, FLT *F) {
	FLT f[] = {1, t, t * t / 2., 0, 1, t, 0, 0, 1};

	memcpy(F, f, sizeof(FLT) * 9);
}

void survive_imu_tracker_init(SurviveIMUTracker *tracker, SurviveObject *so) {
	memset(tracker, 0, sizeof(*tracker));

	tracker->so = so;

	struct SurviveContext *ctx = tracker->so->ctx;
	SV_INFO("Initializing Filter:");
	// These are relatively high numbers to seed with; we are essentially saying
	// origin has a variance of 10m; and the quat can be varied by 4 -- which is
	// more than any actual normalized quat could be off by.

	tracker->pos_Q_per_sec[8] = 1.;

	survive_attach_configf(tracker->so->ctx, VELOCITY_POSITION_VARIANCE_SEC_TAG, &tracker->pos_Q_per_sec[4]);
	survive_attach_configf(tracker->so->ctx, VELOCITY_ROT_VARIANCE_SEC_TAG, &tracker->rot_Q_per_sec[3]);

	survive_attach_configf(tracker->so->ctx, OBS_VELOCITY_POSITION_VAR_TAG, &tracker->obs_variance);
	survive_attach_configf(tracker->so->ctx, OBS_VELOCITY_ROTATION_VAR_TAG, &tracker->obs_rot_variance);

	tracker->acc_bias = 1;
	survive_attach_configf(tracker->so->ctx, POSE_POSITION_VARIANCE_SEC_TAG, &tracker->pos_Q_per_sec[0]);
	survive_attach_configf(tracker->so->ctx, POSE_ROT_VARIANCE_SEC_TAG, &tracker->rot_Q_per_sec[0]);

	survive_attach_configf(tracker->so->ctx, IMU_MAHONY_VARIANCE_TAG, &tracker->mahony_variance);
	survive_attach_configi(tracker->so->ctx, USE_OBS_VELOCITY_TAG, &tracker->use_obs_velocity);

	survive_attach_configf(tracker->so->ctx, IMU_ACC_VARIANCE_TAG, &tracker->acc_var);
	survive_attach_configf(tracker->so->ctx, IMU_GYRO_VARIANCE_TAG, &tracker->gyro_var);

	// size_t dims, const FLT *F, const FLT *Q_per_sec, FLT *P,
	//                               size_t state_size, FLT *state

	survive_kalman_state_init(&tracker->rot, 2, rot_f, tracker->rot_Q_per_sec, 0, 3, 0);
	survive_kalman_state_init(&tracker->position, 3, pos_f, tracker->pos_Q_per_sec, 0, 3, 0);

	SV_INFO("\t%s: %f", POSE_POSITION_VARIANCE_SEC_TAG, tracker->pos_Q_per_sec[0]);
	SV_INFO("\t%s: %f", POSE_ROT_VARIANCE_SEC_TAG, tracker->rot_Q_per_sec[0]);
	SV_INFO("\t%s: %f", VELOCITY_POSITION_VARIANCE_SEC_TAG, tracker->pos_Q_per_sec[4]);
	SV_INFO("\t%s: %f", VELOCITY_ROT_VARIANCE_SEC_TAG, tracker->rot_Q_per_sec[3]);
	SV_INFO("\t%s: %f", IMU_ACC_VARIANCE_TAG, tracker->acc_var);
	SV_INFO("\t%s: %f", IMU_GYRO_VARIANCE_TAG, tracker->gyro_var);
	SV_INFO("\t%s: %f", IMU_MAHONY_VARIANCE_TAG, tracker->mahony_variance);
}

SurviveVelocity survive_imu_velocity(const SurviveIMUTracker *tracker) {
	SurviveVelocity rtn = {0};
	survive_kalman_get_state(0, &tracker->position, 1, rtn.Pos);
	survive_kalman_get_state(0, &tracker->rot, 1, rtn.AxisAngleRot);
	return rtn;
}

void survive_imu_tracker_integrate_velocity(SurviveIMUTracker *tracker, survive_timecode timecode, const FLT *Rv,
											const SurviveVelocity *vel) {
	const FLT H[] = {0, 1, 0};
	FLT time_diff = survive_timecode_difference(timecode, tracker->obs_kalman_update) / (FLT)tracker->so->timebase_hz;

	survive_kalman_predict_update_state(time_diff, &tracker->position, vel->Pos, H, Rv[0]);
	survive_kalman_predict_update_state(time_diff, &tracker->rot, vel->AxisAngleRot, H, Rv[1]);

	tracker->imu_kalman_update = tracker->obs_kalman_update = timecode;
}
