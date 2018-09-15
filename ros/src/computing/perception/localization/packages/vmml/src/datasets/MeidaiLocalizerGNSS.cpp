/*
 * MeidaiLocalizerGNSS.cpp
 *
 *  Created on: Aug 17, 2018
 *      Author: sujiwo
 */

#include <datasets/MeidaiBagDataset.h>
#include <iostream>
#include <string>
#include <vector>

#include <nmea_msgs/Sentence.h>
#include <gnss/geo_pos_conv.hpp>


using namespace std;


const Vector3d
	GNSS_Translation_Offset (18138, 93634, -39);


class wrong_nmea_sentence : public exception
{};


struct GnssLocalizerState
{
	double roll_=0, pitch_=0, yaw_=0;
	double orientation_time_=0, position_time_=0;
	double latitude=0, longitude=0, height=0;
	ros::Time current_time_=ros::Time(0), orientation_stamp_=ros::Time(0);
	geo_pos_conv geo, last_geo;
};


std::vector<std::string> splitSentence(const std::string &string)
{
	std::vector<std::string> str_vec_ptr;
	std::string token;
	std::stringstream ss(string);

	while (getline(ss, token, ','))
		str_vec_ptr.push_back(token);

	return str_vec_ptr;
}


void convertNMEASentenceToState (nmea_msgs::SentencePtr &msg, GnssLocalizerState &state)
{
	vector<string> nmea = splitSentence(msg->sentence);
	if (nmea.at(0).compare(0, 2, "QQ") == 0)
	{
		state.orientation_time_ = stod(nmea.at(3));
		state.roll_ = stod(nmea.at(4)) * M_PI / 180.;
		state.pitch_ = -1 * stod(nmea.at(5)) * M_PI / 180.;
		state.yaw_ = -1 * stod(nmea.at(6)) * M_PI / 180. + M_PI / 2;
		state.orientation_stamp_ = msg->header.stamp;
	}

	else if (nmea.at(0) == "$PASHR")
	{
		state.orientation_time_ = stod(nmea.at(1));
		state.roll_ = stod(nmea.at(4)) * M_PI / 180.;
		state.pitch_ = -1 * stod(nmea.at(5)) * M_PI / 180.;
		state.yaw_ = -1 * stod(nmea.at(2)) * M_PI / 180. + M_PI / 2;
	}

	else if(nmea.at(0).compare(3, 3, "GGA") == 0)
	{
		try {
			state.position_time_ = stod(nmea.at(1));
			state.latitude = stod(nmea.at(2));
			state.longitude = stod(nmea.at(4));
			state.height = stod(nmea.at(9));
			state.geo.set_llh_nmea_degrees(state.latitude, state.longitude, state.height);
		} catch (std::invalid_argument &e) {
			throw wrong_nmea_sentence();
		}
	}

	else if(nmea.at(0) == "$GPRMC")
	{
		state.position_time_ = stoi(nmea.at(1));
		state.latitude = stod(nmea.at(3));
		state.longitude = stod(nmea.at(5));
		state.height = 0.0;
		state.geo.set_llh_nmea_degrees(state.latitude, state.longitude, state.height);
	}

	else
		throw wrong_nmea_sentence();
}


PoseTimestamp createFromState(const GnssLocalizerState &state)
{
	TQuaternion q(state.roll_, state.pitch_, state.yaw_);
	Vector3d p(state.geo.x(), state.geo.y(), state.geo.z());
	p = p + GNSS_Translation_Offset;
	Pose pt = Pose::from_Pos_Quat(p, q);
	return pt;
}


void createTrajectoryFromGnssBag (RandomAccessBag &bagsrc, Trajectory &trajectory, int plane_number)
{
	const double orientationTimeout = 10.0;

	if (bagsrc.getTopic() != "/nmea_sentence")
		throw runtime_error("Not GNSS bag");

//	geo_pos_conv geoconv, last_geo;
	GnssLocalizerState state;
	state.geo.set_plane(plane_number);

	trajectory.clear();

	for (uint32_t ix=0; ix<bagsrc.size(); ix++) {
		cout << ix << "/" << bagsrc.size() << "         \r";

		auto currentMessage = bagsrc.at<nmea_msgs::Sentence>(ix);
		ros::Time current_time = currentMessage->header.stamp;

//		cout << currentMessage->sentence << endl;
//		continue;

		try {
			convertNMEASentenceToState(currentMessage, state);
		} catch (const wrong_nmea_sentence &e)
		{ continue; }

		if (fabs(state.orientation_stamp_.toSec() - currentMessage->header.stamp.toSec()) > orientationTimeout) {
			double dt = sqrt(pow(state.geo.x() - state.last_geo.x(), 2) + pow(state.geo.y() - state.last_geo.y(), 2));
			const double threshold = 0.2;
			if (dt > threshold) {
				// create fake orientation
				state.yaw_ = atan2(state.geo.x() - state.last_geo.x(), state.geo.y() - state.last_geo.y());
				state.roll_ = 0;
				state.pitch_ = 0;
				PoseTimestamp px = createFromState(state);
				px.timestamp = current_time;
				trajectory.push_back(px);
				state.last_geo = state.geo;
				continue;
			}
		}

		double e = 1e-2;
		if (fabs(state.orientation_time_ - state.position_time_) < e) {
			PoseTimestamp px = createFromState(state);
			px.timestamp = current_time;
			trajectory.push_back(px);
		}
	}

	cout << "\nDone\n";
}


PoseTimestamp
PoseTimestamp::interpolate(
	const PoseTimestamp &p1,
	const PoseTimestamp &p2,
	const ros::Time &t)
{
	assert (p1.timestamp>=t and t<=p2.timestamp);
	double r = (t - p1.timestamp).toSec() / (p2.timestamp - p1.timestamp).toSec();
	return Pose::interpolate(p1, p2, r);
}
