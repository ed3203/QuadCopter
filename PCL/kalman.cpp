//============================================================================
// Name        : Test.cpp
// Author      : Chinemelu Ezeh
// Version     :
// Copyright   : Your copyright notice
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <Eigen/Dense>
#include <iostream>
#include <math.h>
#include <vector>
#include <time.h>
#include <Eigen/StdVector>
#include <boost/thread/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "Vision.h"
#include "SerialClass.h"
//#include "serialib.h"

using namespace std;
using namespace Eigen;

#define PLANEGUN
//#define ON_QUAD

typedef Matrix< float , 16 , 16> Matrix16f;
typedef Matrix< float , 16 , 1> Vector16f;
#define STATENUM 16
#define Sampling_Time 0.01	// unit is seconds.
//#define distThreshold 5000
int distThreshold = 35;
#define startupConvergeTimesteps 1000
//#define XtionCovarFudge 100
int XtionCovarFudge = 10000;
#define regmult 1

struct imuRawS
{
	int accX;
	int accY;
	int accZ;
	int gyroX;
	int gyroY;
	int gyroZ;

};

struct planeStruct {
	Vector4f plane;
	Matrix4f cov;
};

class association_struct{
public:
	association_struct() {
		planeId = -1;
		distance = 2e45;
	}
	//-1 means it is not matched.
	int planeId;
	float distance;
	MatrixXf H_opt;
	Matrix4f S;
	// measurement error in plane observation.
	Vector4f m_error;
};


// Function definitions.
Matrix3f DCM_fn();
void initialisation ();
void state_prediction ();
MatrixXf Xi_fn();
MatrixXf noiseMatrix();
Matrix16f F_fn();
MatrixXf G_fn();
void covariance_prediction();
MatrixXf Xip_fn();
MatrixXf H_fn(int planeId);
void getNewMeasurement();
bool getNewMeasurementThalamus();
void getNewObservation();
void getNewObservationLive(QCVision& vision);
void processObservation(bool regActive);
void update(const association_struct& data);
association_struct dataAssociation(const planeStruct& planeData);
void updateSonar();
void controlCraft();
//void run();

//Variables
vector<Vector4f, Eigen::aligned_allocator<Vector4f> > landmarks;
//vector<Vector16f, Eigen::aligned_allocator<Vector16f> > statehistory;
vector<planeStruct, Eigen::aligned_allocator<planeStruct> > newPlanes;

Vector3f acc;
Vector3f gyro;
float sonarAlt;
float sonarVariance = 0.02*0.02;
bool newsonar = false;

Vector16f state;
MatrixXf P(16,16);
MatrixXf bigH;
Matrix3f DCM;
MatrixXf Xi(4,3);
MatrixXf Xip(4,3);
MatrixXf Q(12,12);

int timeSteps = 0;

int kalman(QCVision& vision) {
	clock_t tStart = clock();

	vision.DCM = &DCM;
	vision.stateVector = &state;
	
	initialisation ();

	while(true) {

		while(!getNewMeasurementThalamus()){
			//cout <<"wait"<<endl;
			boost::this_thread::sleep(boost::posix_time::millisec(2));
		}

		DCM = DCM_fn();
		Xi = Xi_fn();

		state_prediction();
		covariance_prediction();

		if (newsonar)
		{
			updateSonar();
		}

		vision.m_mutexLockPlanes.lock();
		if (vision.bNewSetOfPlanes){
			getNewObservationLive(vision);
			vision.bNewSetOfPlanes = false;
			vision.m_mutexLockPlanes.unlock();
			processObservation(true); //timeSteps > startupConvergeTimesteps);
			cout << "state at time t = " << timeSteps << endl<< state.segment(0,3) << endl<<endl;
			cout << "numplanes: " << landmarks.size() << endl;

		} else {
			vision.m_mutexLockPlanes.unlock();
		}

		controlCraft();

		timeSteps++;
	}

//	for (int i = 0; i < (int)statehistory.size(); i++)
//		cout << "state at time t = " << i << endl<< statehistory[i] << endl<<endl;
	printf("Time taken: %.2fs\n", (double)(clock() - tStart)/CLOCKS_PER_SEC);
	return 0;
}

void initialisation () { //Incomplete.

	//Initialise landmarks.
	Vector4f planeCloud_t;
	planeCloud_t << -1,0,0,0;
	//planeCloud_t << 1,0,0,0;
	landmarks.push_back(planeCloud_t);
	planeCloud_t << 0,1,0,0;
	landmarks.push_back(planeCloud_t);
	planeCloud_t << 0,0,1,0;
	//planeCloud_t << 0,0,-1,0;
	landmarks.push_back(planeCloud_t);

	//initialise landmarks and P
	Matrix3f block1 = Matrix3f::Identity();
	Matrix4f block2 = Matrix4f::Identity();
	P << 	block1*0.09, MatrixXf::Zero(3,13),
		MatrixXf::Zero(3,16),
		MatrixXf::Zero(4,6), block2*0.01, MatrixXf::Zero(4,6),
		MatrixXf::Zero(3,10), block1*0.0025, MatrixXf::Zero(3,3),
		MatrixXf::Zero(3,13), block1;
		//MatrixXf::Zero(12,28);
	//	cout << "P initial" << endl << P << endl << endl;
	Q = noiseMatrix();
	//initialise state vector.
	state <<-2.2, 2.2, 2.2, 0, 0, 0, 0.853553, 0.146447, 0.353553, -0.353553,
	//state << 1, 1, -1, 0, 0, 0, 0.353553, -0.353553, -0.146447, -0.853553, 
		-0.0456 ,   0.0069,   -0.0048 ,  -0.0331  ,  0.1024 ,   0.1473;
	//	cout << "state initial" << endl << state << endl << endl;

	sonarAlt = state(2);
}

Matrix3f DCM_fn() { //There is a round off error in dcm(2,3). May cause an issue.
	Vector4f q;
	q << state.segment(6,4);
	Matrix3f dcm;
	dcm << 	2*(pow(q(0),2) + pow(q(1),2)) - 1,		2*(q(1)*q(2) + q(0)*q(3)), 		2*(q(1)*q(3) - q(0)*q(2)),
		2*(q(1)*q(2) - q(0)*q(3)), 				2*(pow(q(0),2) + pow(q(2),2)) - 1, 2*(q(2)*q(3) + q(0)*q(1)),
		2*(q(1)*q(3) + q(0)*q(2)), 2*(q(2)*q(3) - q(0)*q(1)), 2*(pow(q(0),2) + pow(q(3),2)) - 1;
	//	cout << "dcm" << endl << dcm << endl << endl;
	return dcm;
}

// Calculates Predicted Next State.
void state_prediction () {

	/*
	* state-> 	0:2 position
	* 			3:5 velocity
	* 			6:9 quaternions
	* 			10:12 gyro bias
	* 			13:15 acc bias
	*/
	//Get Xdelta.

	Vector16f xdelta; //dynamic allocation maybe.
	//Position
	xdelta(0) = state(3);
	xdelta(1) = state(4);
	xdelta(2) = state(5);

	//Velocity
	//initialize the DCM using quaternions from state.
	xdelta.segment(3,3) << DCM.transpose() *  acc;
	xdelta(5) += 9.816;

	//Quaternions
	/*
	* To compute quaternion transition, first compute angular velocity vector
	*/

	xdelta.segment(6,4) << 0.5 * Xi * gyro;
	xdelta.segment(10,6) << 0,0,0,0,0,0;
	//Biases
	//xdelta.block<6,1>(10,1) = 0;
	//	cout << "xdelta" << endl << xdelta << endl<<endl;
	state += Sampling_Time*xdelta;
	//normalise quaternions.
	state.segment(6,4)/=state.segment(6,4).norm();
	//	cout << "state predicted" << endl << state << endl<<endl;
}
MatrixXf Xi_fn() {
	MatrixXf Xi_t(4,3);
	Vector4f q(state.segment(6,4));
	Xi_t << -q(1), -q(2), -q(3),
		q(0), -q(3), q(2),
		q(3), q(0), -q(1),
		-q(2), q(1), q(0);
	//	cout << "Xi_t" << endl << Xi_t << endl << endl;
	return Xi_t;
}
MatrixXf noiseMatrix() {
	Matrix<float, 12, 12> Q_t;
	Matrix3f zeros3(Matrix<float, 3, 3>::Zero());
	Matrix3f Qgyro;
	Matrix3f Qacc;
	Matrix3f Qgyrobias;
	Matrix3f Qaccbias;

	Qgyro << 	0.1679,   -0.0615,   -0.1051,
		-0.0615,    0.1320,   -0.0654,
		-0.1051,   -0.0654,    0.2536;
	Qgyro *= 1.0e-03;


	Qacc << 	0.6670,    0.0377,   -0.0648,
		0.0377,    0.8222,   -0.1422,
		-0.0648,   -0.1422,    2.7314;
	Qacc *= 1.0e-03;


	Qgyrobias << 	0.3552,   -0.0323,   -0.2509,
		-0.0323,    0.2822,   -0.2381,
		-0.2509,   -0.2381,    0.5686;
	Qgyrobias *= 1.0e-03;


	Qaccbias << 	0.3501,   -0.1312,    0.2149,
		-0.1312,    0.4719,   -0.2273,
		0.2149,   -0.2273,    1.3670;
	Qaccbias *= 1.0e-02;

	Q_t << 	Qgyro, zeros3, zeros3, zeros3,
		zeros3, Qacc, zeros3, zeros3,
		zeros3, zeros3, Qgyrobias, zeros3,
		zeros3, zeros3, zeros3, Qaccbias;
	//	cout << "Q_t" << endl << Q_t << endl << endl;
	return Q_t;
}

Matrix16f F_fn() {
	// Porting the equations direct from Matlab...
	Vector4f Fvqparts(Xi * acc);

	MatrixXf Fvq(3,4);
	Fvq <<	Fvqparts(1), -Fvqparts(0), Fvqparts(3), -Fvqparts(2),
		Fvqparts(2), -Fvqparts(3), -Fvqparts(0), Fvqparts(1),
		Fvqparts(3), Fvqparts(2), -Fvqparts(1), -Fvqparts(0);
	Fvq*=2;
	/*********************/
	Matrix3f Fvba(-DCM.transpose());
	Vector3f w(gyro);

		Matrix4f omega;
		omega << 0,		-w(0),	-w(1),	-w(2),
				 w(0),	0,		w(2),	-w(1),
				 w(1), 	-w(2), 	0, 		w(0),
				 w(2),	w(1), 	-w(0), 	0;
	Matrix4f Fqq(0.5 * omega);
	MatrixXf Fqbw(-0.5 * Xi);
	/********/
	//Build F matrix
	Matrix16f F;
	Matrix3f eye3(Matrix3f::Identity());
	Matrix3f zeros3(Matrix3f::Zero());

	F << 	zeros3, eye3, Matrix<float, 3, 10>::Zero(),
		zeros3, zeros3, Fvq, zeros3, Fvba,
		Matrix<float, 4, 6>::Zero(), Fqq, Fqbw, Matrix<float, 4, 3>::Zero(),
		Matrix<float, 6, 16>::Zero();
	F *= Sampling_Time;
	F+= Matrix<float, 16, 16>::Identity();
	//	cout << "Xi" << endl << Xi << endl << endl;
	//	cout << "Fvq" << endl << Fvq << endl << endl;
	//	cout << "Fqbw" << endl << Fqbw << endl << endl;
	//	cout << "F" << endl << F << endl << endl;
	return F;
}

MatrixXf G_fn() {
	MatrixXf Gqww(0.5 * Xi);
	Matrix3f Gvwa(DCM.transpose());
	Matrix <float, 16, 12>G;
	G << Matrix<float, 3, 12>::Zero(),
		Matrix<float, 3, 3>::Zero(), Gvwa, Matrix<float, 3, 6>::Zero(),
		Gqww, Matrix<float, 4, 9>::Zero(),
		Matrix<float, 3, 6>::Zero(), Matrix<float, 3, 3>::Identity(), Matrix<float, 3, 3>::Zero(),
		Matrix<float, 3, 9>::Zero(), Matrix<float, 3, 3>::Identity();
	G *= Sampling_Time;
	//	cout << "G" << endl << G << endl << endl;
	return G;
}
void covariance_prediction() {
	MatrixXf G(G_fn());
	MatrixXf F(F_fn());
	P = F * P * F.transpose() + G * Q * G.transpose();
	// Should we propagate any change to the other blocks of P?
	/*
	if (P.cols() > 16) {
		MatrixXf P_temp(F*P.block(0,16, 16, P.cols() - 16));
		P.block(0,16, 16, P.cols() - 16) = P_temp;
		P.block(16,0, P.rows() - 16,16)  = P_temp.transpose();
	}
	*/
//	cout << "P" << endl << P << endl << endl;

}
MatrixXf Xip_fn() {
	MatrixXf Xip_t(4,3);
	Vector4f q(state.segment(6,4));
	//	cout << "q" << endl << q << endl << endl;
	Xip_t << 	-q(1), -q(2), -q(3),
		-q(0), -q(3),  q(2),
		q(3),  -q(0), -q(1),
		-q(2),  q(1),  -q(0);
	//	cout << "Xip" << endl << Xip_t << endl << endl;
	return Xip_t;
}

MatrixXf H_fn(int planeId) {
	MatrixXf H(4, 16);
	// Build H for each plane.


	Vector4f plane(landmarks[planeId]);
	Vector4f Hqparts;
	Hqparts << Xip * plane.segment(0,3); //Nx,Ny Nz
	//	cout << "Hqparts" << endl << Hqparts << endl << endl;
	//	cout << "currtP" << endl << plane << endl << endl;
	MatrixXf Hq(3,4);
	Hq << -Hqparts(1), -Hqparts(0), Hqparts(3), -Hqparts(2),
		-Hqparts(2), -Hqparts(3), -Hqparts(0), Hqparts(1),
		-Hqparts(3), Hqparts(2), -Hqparts(1), -Hqparts(0);
	Hq*=2;
	//	cout << "Hq" << endl << Hq << endl << endl;
	MatrixXf block(4,3);
	Matrix4f block2;
	//Matrix4f block3(4,4);
	block << MatrixXf::Zero(3,3), plane.segment(0,3).transpose();
	block2 << Hq, Matrix<float, 1, 4>::Zero();

	H << 	block,  	MatrixXf::Zero(4,3), 	block2,		MatrixXf::Zero(4,6);
	//block3;
	//	cout << "H" << endl << H << endl << endl;
	return H;
}

association_struct dataAssociation(const planeStruct& planeData) {

	Matrix4f transPlane;
	transPlane << DCM, Vector3f::Zero(), state.segment(0,3).transpose(), 1;

	Vector4f inP(planeData.plane);
	Matrix4f inC(planeData.cov);
	association_struct data;
	MatrixXf S;
	Vector4f diff;
	float dist;

	bool first = true;

	for (int index = 0; index < (int)landmarks.size(); index++) {
		MatrixXf H(H_fn(index));

		//	Build the optimised P.
		S = H * P * H.transpose() + inC;
		diff = inP - transPlane*landmarks[index];
		dist = diff.transpose() *  S.inverse() * diff;
		cout << "dist " << endl << dist << endl << endl;

		if ((first == true) || (dist < data.distance)) {
			first = false;
			data.distance = dist;
			data.H_opt = H;
			data.S = S;
			data.m_error = diff;
			data.planeId = index;
			//cout << "diff " << endl << diff << endl << endl;
			//cout << "dist " << endl << dist << endl << endl;
		}
	}

	return data;
}

void processObservation(bool regActive) {
	//Data Association.

	for (int i = 0; i < (int)newPlanes.size(); i++) {
		// Re-evaluate Important functions based on robot state.
		DCM = DCM_fn();
		Xip = Xip_fn();
		Xi = Xi_fn();
		//Perform association.
		association_struct data = dataAssociation(newPlanes[i]);
		//Either register plane or update kalman equations
		if (data.distance <= distThreshold || !regActive) {	//Data was associated!
			//cout << "updated lm: "<< data.planeId << endl<<endl;
			update(data);
		} else {	// Data NOT associated so register new plane.
			cout << "-------------------------------------------------------NOISE" <<endl;
		}
	}
}

void update(const association_struct& data) {
	// Unpack data.
	MatrixXf H_opt(data.H_opt);
	Matrix4f S(data.S);
	Vector4f diff = data.m_error;
	int index = data.planeId;

	MatrixXf kalmanGain(P * H_opt.transpose() * S.inverse());

	state += kalmanGain * diff;
	//Normalise Quaternions.
	state.segment(6,4) /= state.segment(6,4).norm();

	//P -= kalmanGain * S * kalmanGain.transpose();
	P = (Matrix<float, 16, 16>::Identity() - kalmanGain * H_opt) * P;

//	cout << "change " << change << endl << endl;
//	cout << "P: " << P << endl << endl;
//	cout << "H_opt.transpose" << endl << H_opt.transpose() << endl << endl;
//	cout << "S.inverse" << endl << S.inverse() << endl << endl;
//	cout << "kalmanGain" << endl << kalmanGain << endl << endl;
//	cout << "State Update to landmark " << index <<";" << endl << state << endl <<"Update Above to landmark: "<< index << endl <<endl;
//	cout << "Distance" << endl << data.distance << endl << endl;
}


void updateSonar(){
	float y = sonarAlt - state(2);
	float s = P(2,2) + sonarVariance;
	VectorXf K = P.col(2) / s;
	state += K*y;
	//P -= K*s*K.transpose();
	P -= K*P.row(2);
}


#ifdef PLANEGUN

void controlCraft(){
	//do nothing here
}

bool getNewMeasurementThalamus(){
	static Serial SP("\\\\.\\COM62");
	int SPba = SP.BytesAvailable();
	//int SPba = sp.Peek();
	if (SPba >= 10*2)
	{
		char sync[2];
		
		SP.ReadData(sync,sizeof(sync));

		if (sync[0] == 11)
		{
			short GyroRaw[3];
			SP.ReadData((char*)GyroRaw,sizeof(GyroRaw));

			Vector3f gyro_t;
			gyro_t << GyroRaw[0], GyroRaw[1], GyroRaw[2];
			gyro_t /= 818.51113590117601252569;
			gyro << gyro_t(2), -gyro_t(0), -gyro_t(1);
			Vector3f gyroBias(state.segment(10,3));
			//	cout << "gyro measured" << endl << gyro << endl << endl;
			gyro  -= gyroBias;


			short AccRaw[3];
			SP.ReadData((char*)AccRaw,sizeof(AccRaw));

			Vector3f acc_t;
			acc_t << AccRaw[0], AccRaw[1], AccRaw[2];
			acc_t*=(9.816 /pow(2.00,14));
			acc << acc_t(2), -acc_t(0), -acc_t(1);
			Vector3f accBias(state.segment(13,3));
			//	cout << "acc measured" << endl << acc << endl << endl;
			acc-=accBias;

			short MagRaw[3];
			SP.ReadData((char*)MagRaw,sizeof(MagRaw));

			return true;
		}
	}

	return false;
}

#endif

//#ifdef ON_QUAD

void controlCraft(){
	Vector2f horizSetpoint(3,3);
	Vector2f currPos(state.segment<2>(0));
	Vector2f posErr = horizSetpoint - currPos;

	//rotate the error into the crafts frame, then project only the x and y axies onto the horizontal plane
	Matrix2f DCM2D = DCM_fn().block<2,2>(0,0);
	Vector2f posErrBody = DCM2D * posErr;
	Vector2f velBody = DCM2D * state.segment<2>(3);

}

//#endif


void getNewObservationLive(QCVision& vision){
	newPlanes.clear();
	Matrix4f hypermute;
			hypermute << 	0,0,1,0,
							1,0,0,0,
							0,1,0,0,
							0,0,0,1;

	vector<Plane> pv = vision.getPlanes();

	for (int i = 0; i < pv.size(); ++i)
	{
		Plane& currplane = pv[i];
		vector<vector<float> >& inC = currplane.covariance;
		Vector4f planeCloud_t;
		Matrix4f cov_t;


		planeCloud_t << currplane.A, currplane.B, currplane.C, currplane.D;

		cov_t << inC[0][0], inC[0][1], inC[0][2], inC[0][3],
				 inC[1][0], inC[1][1], inC[1][2], inC[1][3],
				 inC[2][0], inC[2][1], inC[2][2], inC[2][3],
				 inC[3][0], inC[3][1], inC[3][2], inC[3][3];

		planeStruct temp;
		temp.cov = hypermute*(cov_t*XtionCovarFudge)* hypermute.transpose();
		temp.plane = hypermute*planeCloud_t;
		newPlanes.push_back(temp);
	}
}

