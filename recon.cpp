#include "recon.hpp"
#include <stdio.h> //DEBUG
#include <string.h> //DEBUG

int main(int argc, char ** argv) {
	//načti všechny body
	Configuration config = Configuration(argc, argv);
	Heuristic hint(&config);
	Render *render = spawnRender(hint);
	Mat points = config.reconstructedPoints();
	
	Mat indices = alphaShapeIndices(points); // FIXME DEBUG FAIL WTF
	while (hint.notHappy(points)) {
		//sestav z nich alphashape
		printf("converted %i %id points into %i face indices\n", points.rows, points.cols, indices.rows);
		render->loadMesh(points, indices);

		hint.chooseCameras();
		for (int fa = hint.beginMain(); fa != Heuristic::sentinel; fa = hint.nextMain()) {
			//vyber náhodné dva snímky
			//promítni druhý do kamery prvního
			Mat originalImage = config.frame(fa);
			/*
			Mat projectedPoints = config.camera(fa) * points.t();
			for (int i=0; i<projectedPoints.cols; i++) {
				float pointW = projectedPoints.at<float>(3, i), pointX = projectedPoints.at<float>(0, i)/pointW, pointY = projectedPoints.at<float>(1, i)/pointW, pointZ = projectedPoints.at<float>(2, i)/pointW;
				cv::Scalar color = (pointZ <= 1 && pointZ >= -1) ? cv::Scalar(128*(1-pointZ), 128*(pointZ+1), 0) : cv::Scalar(0, 0, 255);
				cv::circle(originalImage, cv::Point(originalImage.cols*(0.5 + pointX*0.5), originalImage.rows * (0.5 - pointY*0.5)), 3, color, -1, 8);
			}*/
			char filename[300];
			snprintf(filename, 300, "frame%i.png", fa);
			saveImage(originalImage, filename); //DEBUG
			Mat depth = render->depth(config.camera(fa));
			snprintf(filename, 300, "frame%idepth.png", fa);
			saveImage(depth, filename, true); //DEBUG
			MatList flows, cameras;
			for (int fb = hint.beginSide(fa); fb != Heuristic::sentinel; fb = hint.nextSide(fa)) {
				Mat projectedImage = render->projected(config.camera(fa), config.frame(fb), config.camera(fb));
				mixBackground(projectedImage, originalImage, depth);
				//nahrubo ulož výsledek
				snprintf(filename, 300, "frame%ifrom%ip.png", fa, fb);
				saveImage(projectedImage, filename); //DEBUG
				Mat flow = calculateFlow(originalImage, projectedImage);
				mixBackground(flow, Mat::zeros(flow.rows, flow.cols, CV_32FC2), depth);
				snprintf(filename, 300, "frame%ifrom%if.png", fa, fb);
				saveImage(flow, filename, true);
				flows.push_back(flow);
				cameras.push_back(config.camera(fb));
				break;
			}
			//trianguluj všechny pixely
			points.push_back(triangulatePixels(flows, config.camera(fa), cameras, depth));
			printf("%i points\n", points.rows);
			break;
		}
		hint.filterPoints(points);
	}
	delete render;
	//vysypej triangulované body jako obj
	//Mat indices = alphaShapeIndices(points);
	//printf("%i faces\n", indices.rows);
	saveMesh(points, indices, "triangulated.obj");
	return 0;
}
