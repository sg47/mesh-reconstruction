#include <opencv2/flann/flann.hpp>
#include "recon.hpp"

typedef cvflann::L2_Simple<float> Distance;
typedef std::pair<int, float> Neighbor;

Heuristic::Heuristic(Configuration *iconfig)
{
	config = iconfig;
	iteration = 0;
}

bool Heuristic::notHappy(const Mat points)
{
	iteration ++;
	return (iteration <= 1);
}

const inline float pow2(float x)
{
	return x * x;
}

const inline float densityFn(float dist, float radius)
{
	return (1. - dist/radius);
}

void Heuristic::filterPoints(Mat& points)
{
	printf("Filtering: Preparing neighbor table...\n");
	int pointCount = points.rows;
	Mat points3 = dehomogenize(points);
	
	const float radius = alphaVals.back()/4.;
	std::vector<int> neighborBlocks(pointCount+1, 0);
	std::vector<Neighbor> neighbors;
	neighbors.reserve(pointCount);
	// Připrav tabulku sousedů
	{
		cv::flann::GenericIndex<Distance> index = cv::flann::GenericIndex<Distance>(points3, cvflann::KDTreeIndexParams());
		cvflann::SearchParams params;
		std::vector<float> distances(pointCount, 0.), point(3, 0.);
		std::vector<int> indices(pointCount, -1);
		for (int i=0; i<pointCount; i++) {
			points3.row(i).copyTo(point);
			index.radiusSearch(point, indices, distances, radius, params);
			int writeIndex = 0;
			neighborBlocks[i] = neighbors.size();
			int end;
			for (end = 0; end < indices.size() && indices[end] >= 0; end++);
			for (int j = 0; j < end; j++) {
				if (indices[j] < i && distances[j] <= radius) // pro zaručení symetrie se berou jen předcházející sousedi
					neighbors.push_back(Neighbor(indices[j], densityFn(distances[j], radius)));
				indices[j] = -1;
			}
		}
	}
	neighborBlocks[pointCount] = neighbors.size();
	printf(" Neighbors total: %lu, %f per point.\n", neighbors.size(), ((float)neighbors.size())/pointCount);
	
	printf("Estimating local density...\n");
	// Spočítej hustotu bodů v okolí každého (vlastní vektor pomocí power iteration)
	std::vector<float> density(pointCount, 1.), densityNew(pointCount, 0.);
	double change;
	int densityIterationNo = 0;
	do {
		for (int i=0; i<pointCount; i++) {
			densityNew[i] = 0.;
		}
		double sum = 0.;
		for (int i=0; i<pointCount; i++) {
			// přičti za každého souseda jeho význam vážený podle vzdálenosti
			float densityTemp = 0.0;
			for (int j = neighborBlocks[i]; j < neighborBlocks[i+1]; j++) {
				densityTemp += density[neighbors[j].first] * neighbors[j].second;
				densityNew[neighbors[j].first] += density[i] * neighbors[j].second;
				sum += (density[i] + density[neighbors[j].first]) * neighbors[j].second;
			}
			densityNew[i] += densityTemp;
		}
		float normalizer = pointCount / sum;
		change = 0.;
		for (int i=0; i<pointCount; i++) {
			float normalizedDensity = densityNew[i] * normalizer;
			if (normalizedDensity > 2.)
				normalizedDensity = 2.; // oříznutí -- jinak by maximální hustota dosahovala tisíců
			change += pow2(density[i] - normalizedDensity);
			density[i] = normalizedDensity;
		}
		change /= pointCount;
		densityIterationNo += 1;
	} while (change > 1e-6);
	float densityLimit = 0.0;
	for (int i=0; i<pointCount; i++) {
		densityNew[i] = density[i];
		if (density[i] > densityLimit)
			densityLimit = density[i];
	}
	densityLimit = .7;
	printf(" Density converged in %i iterations. Limit set to: %f\n", densityIterationNo, densityLimit);
	//projdi kandidáty od nejhustších, poznamenávej si je jako přidané a dle libovůle snižuj hustotu okolním
	std::vector<int> order(pointCount, -1);
	cv::sortIdx(density, order, cv::SORT_DESCENDING);
	int writeIndex = 0;
	for (int i=0; i<pointCount; i++) {
		int ord = order[i];
		if (densityNew[ord] < densityLimit)
			continue;
		// odečti hustotu, zbav se sousedů (odečti původní hustotu, protože nová může být záporná)
		double localDensity = density[ord];
		for (int j=neighborBlocks[ord]; j<neighborBlocks[ord+1]; j++) {
			densityNew[neighbors[j].first] -= localDensity * neighbors[j].second;
		}
		if (i > writeIndex)
			order[writeIndex] = order[i];
		writeIndex ++;
	}
	//fakticky vyfiltruj z matice všechny podhuštěné body
	std::sort(order.begin(), order.begin() + writeIndex);
	for (int i=0; i<writeIndex; i++) {
		if (order[i] > i)
			points.row(order[i]).copyTo(points.row(i)); //to můžu udělat díky tomu, že jsem indexy setřídil
	}
	points.resize(writeIndex);
}
/*
void Heuristic::filterPoints(Mat points)
{
	int pointCount = points.rows;
	Mat points3(0, 3, CV_32F);
	for (int i=0; i<pointCount; i++) {
		points3.push_back(points.row(i).colRange(0,3) / points.at<float>(i, 3));
	}
	//Mat centers;
	//cv::flann::hierarchicalClustering<float> (points3, centers, const cvflann::KMeansIndexParams& params, Distance d=Distance())
	for (int i=1; 10*i < points.rows; i++) {
		points.row(10*i).copyTo(points.row(i));
	}
	points.resize(points.rows/10);
	printf("%i filtered points\n", points.rows);
}
*/
float faceArea(Mat points, int ia, int ib, int ic)
{
	Mat a = points.row(ia),
	    b = points.row(ib),
	    c = points.row(ic);
	a = a.colRange(0,3) / a.at<float>(3);
	b = b.colRange(0,3) / b.at<float>(3);
	c = c.colRange(0,3) / c.at<float>(3);
	Mat e = b-a,
	    f = c-b;
	return cv::norm(e.cross(f))/2;
}

const Mat faceCamera(const Mat points, const Mat indices, int faceIdx, float far)
{
	const int32_t *vertIdx = indices.ptr<int32_t>(faceIdx);
	Mat a(points.row(vertIdx[0])), b(points.row(vertIdx[1])), c(points.row(vertIdx[2]));
	a = a.colRange(0,3) / a.at<float>(3);
	b = b.colRange(0,3) / b.at<float>(3);
	c = c.colRange(0,3) / c.at<float>(3);
	Mat normal((b-a).cross(c-b)),
	    center((a+b+c)/3);
	float normalLength = cv::norm(normal);
	normal /= normalLength;

	Mat RT;
	// ready, steady...
	float *n = normal.ptr<float>(0),
	      *ce = center.ptr<float>(0);
	float x = n[0], y = n[1], z = n[2];
	float xys = x*x + y*y, xy = sqrt(xys),
	      dot = center.dot(normal);
	// ...go!
	if (xy > 0) {
		RT = Mat(cv::Matx44f(
			x*z,  y*z,  -xys, ce[2]*xys - z*dot,
			-y,   x,    0,    y*ce[1]-x*ce[0],
			x*xy, y*xy, z*xy, -xy*dot,
			0,    0,    0,    xy));
	} else { // no need for rotation
		float s = (z > 0) ? 1 : -1;
		RT = Mat(cv::Matx44f(
			1, 0, 0, -ce[0],
			0, s, 0, -ce[1],
			0, 0, s, -ce[2],
			0, 0, 0, 1));
	}

	float focal = 0.25, // focal length
	      near = 0.001;//normalLength/4; // just a value with length units...
	Mat K(cv::Matx44f(
		focal, 0, 0, 0,
		0, focal, 0, 0,
		0, 0, (near+far)/(far-near), 2*near*far/(near-far),
		0, 0, 1, 0));
	return K*RT;
}

int bisect(std::vector<float> list, float choice)
{
	//whatever.
	for (int i=0; i<list.size(); i++) {
		if (list[i] > choice)
			return i-1;
	}
	return list.size();
}

int myFind(std::vector<numberedVector> list, int index)
{
	//return -1 if index not in list
	//else return i: list[i].first == index
	for (int i=0; i<list.size(); i++) {
		if (list[i].first == index)
			return i;
	}
	return -1;
}

int myFind(std::vector<int> list, int index)
{
	for (int i=0; i<list.size(); i++) {
		if (list[i] == index)
			return i;
	}
	return -1;
}

typedef struct{
	int index;
	float weightFromViewer, weightToViewer, distance;
} CameraLabel;
typedef std::vector< std::pair<CameraLabel, Mat> > LabelledCameras;

LabelledCameras filterCameras(Mat viewer, Mat depth, const std::vector<Mat> cameras)
{
	LabelledCameras filtered;
	{int i=0; for (std::vector<Mat>::const_iterator camera=cameras.begin(); camera!=cameras.end(); camera++, i++) {
		Mat imageOfCameraCenter = Mat(cv::Matx41f(0,0,-1,0)); // ale to přece není pravda.
		Mat camPos = camera->inv()*imageOfCameraCenter;
		Mat cameraFromViewer = viewer * camPos;
		float *cfv = cameraFromViewer.ptr<float>(0);
		cameraFromViewer /= cfv[3];
		cfv = cameraFromViewer.ptr<float>(0);
		if (cfv[0] < -1 || cfv[0] > 1 || cfv[1] < -1 || cfv[1] > 1 || cfv[2] < -1) { //DEBUG: WTF?
			//printf("  Failed test from viewer: %g, %g, %g\n", cfv[0], cfv[1], cfv[2]);
			continue;
		}
		
		int row = (cfv[1] + 1) * depth.rows / 2,
		    col = (cfv[0] + 1) * depth.cols / 2;
		float obstacleDepth = depth.at<float>(row, col);
		if (obstacleDepth != backgroundDepth && obstacleDepth <= cfv[2]) {
			//printf("  Failed depth test: %g >= %g\n", cfv[2], obstacleDepth);
			continue;
		}
		
		Mat viewerPos = viewer.inv()*imageOfCameraCenter;
		Mat viewerFromCamera = *camera * viewerPos;
		float *vfc = viewerFromCamera.ptr<float>(0);
		viewerFromCamera /= vfc[3];
		vfc = viewerFromCamera.ptr<float>(0);
		if (vfc[0] < -1 || vfc[0] > 1 || vfc[1] < -1 || vfc[1] > 1 || vfc[2] < -1) { //DEBUG: WTF?
			//printf("  Failed test from camera: %g, %g, %g\n", vfc[0], vfc[1], vfc[2]);
			continue;
		}
		
		// passed all tests
		CameraLabel label;
		label.index = i;
		label.weightFromViewer = 1 - (cfv[0]*cfv[0] + cfv[1]*cfv[1]);
		if (label.weightFromViewer < 0)
			continue;
		label.weightToViewer = 1 - (vfc[0]*vfc[0] + vfc[1]*vfc[1]);
		if (label.weightToViewer < 0)
			continue;
		label.distance = cfv[2]; //FIXME
		filtered.push_back(std::pair<CameraLabel, Mat>(label, *camera));
	}}
	//printf(" %i cameras passed visibility tests\n", filtered.size());
	return filtered;
}

const CameraLabel* chooseMain(const Mat viewer, const std::vector<numberedVector> chosenCameras, LabelledCameras filteredCameras)
{
	if (filteredCameras.size() == 0)
		return NULL;
	std::vector<float> weightSum(filteredCameras.size()+1, 0.);
	{int i=0; for (LabelledCameras::const_iterator it = filteredCameras.begin(); it != filteredCameras.end(); it++, i++) {
		CameraLabel label = it->first;
		float weight = label.weightToViewer * label.weightFromViewer;
		if (myFind(chosenCameras, label.index) >= 0)
			weight += 50;
		weightSum[i+1] = weightSum[i] + weight;
	}}
	//printf("average weight: %g\n", weightSum.back() / filteredCameras.size());
	float choice = cv::randu<float>() * weightSum.back();
	int index = bisect(weightSum, choice);
	//printf("  I shot at %g from %g and thus decided for main camera %i (at position %i)\n", choice, weightSum.back(), filteredCameras[index].first.index, index);
	return &(filteredCameras[index].first);
}

const CameraLabel* chooseSide(const Mat viewer, const std::vector<int> chosenSideCameras, CameraLabel mainCamera, LabelledCameras filteredCameras)
{
	// TODO: try to pick similar depth as main, but different point of view than other side cameras
	if (filteredCameras.size() == 1) {// mainCamera is surely in filteredCameras and we cannot pick it
		printf(" No side cameras available\n");
		return NULL;
	}
	std::vector<float> weightSum(filteredCameras.size(), 0.);
	std::vector<const CameraLabel*> labels(filteredCameras.size()-1, NULL);
	int i=0;
	for (LabelledCameras::const_iterator it = filteredCameras.begin(); it != filteredCameras.end(); it++) {
		CameraLabel label = it->first;
		if (label.index == mainCamera.index)
			continue;
		float weight = label.weightToViewer * sqrt(1-label.weightFromViewer*label.weightFromViewer);
		if (myFind(chosenSideCameras, label.index) >= 0)
			weight += 5;
		weightSum[i+1] = weightSum[i] + weight;
		labels[i] = &(it->first);
		i++;
	}
	float choice = cv::randu<float>() * weightSum.back();
	int index = bisect(weightSum, choice);
	assert(index < i);
	//printf("  I shot at %g from %g and thus decided for side camera %i (at position %i of %i)\n", choice, weightSum.back(), labels[index]->index, index, i);
	return labels[index];
}

void Heuristic::chooseCameras(const Mat points, const Mat indices, const std::vector<Mat> cameras)
{
	chosenCameras.clear();
	std::vector<float> areaSum(indices.rows+1, 0.); //TODO: this needs not be a vector, array would be OK
	for (int i=0; i<indices.rows; i++) {
		const int32_t *vertIdx = indices.ptr<int32_t>(i);
		areaSum[i+1] = areaSum[i] + faceArea(points, vertIdx[0], vertIdx[1], vertIdx[2]);
	}
	float totalArea = areaSum.back(),
	      average = totalArea / indices.rows;
	
	std::vector<bool> used(false, indices.rows);
	float bullets = 1; // expected count of shots, including misses ~ (frame count)/(bullets)
	cv::RNG random = cv::theRNG();
	Render *render = spawnRender(*this);
	render->loadMesh(points, indices);
	std::vector<int> empty;
	while (1) {
		float choice = cv::randu<float>() * (totalArea + average*bullets);
		if (choice >= totalArea) {
			// congratulations: you won the Russian roulette
			if (chosenCameras.size() > 0)
				break;
			else
				continue;
		} else {
			int chosenIdx = bisect(areaSum, choice);
			//printf(" Projecting from face %i\n", chosenIdx);
			float far = 10; //FIXME: nastavit na nejvzdálenější kameru
			Mat viewer = faceCamera(points, indices, chosenIdx, far);
			Mat depth = render->depth(viewer);
			LabelledCameras filteredCameras = filterCameras(viewer, depth, cameras);
			const CameraLabel *mainCamera = chooseMain(viewer, chosenCameras, filteredCameras);
			if (mainCamera) {
				//printf(" Chosen main camera %i\n", mainCamera->index);
				int positionMain = myFind(chosenCameras, mainCamera->index);
				const CameraLabel *sideCamera;
				if (positionMain != -1)
					sideCamera = chooseSide(viewer, chosenCameras[positionMain].second, *mainCamera, filteredCameras);
				else
					sideCamera = chooseSide(viewer, empty, *mainCamera, filteredCameras);
				if (sideCamera) {
					//printf("  Chosen side camera %i\n", sideCamera->index);
					if (positionMain < 0) {
						chosenCameras.push_back(numberedVector(mainCamera->index, std::vector<int>(1, sideCamera->index)));
					}	else {
						int positionSide = myFind(chosenCameras[positionMain].second, sideCamera->index);
						if (positionSide < 0)
							chosenCameras[positionMain].second.push_back(sideCamera->index);
					}
				}
			}
		}
	}
	//DEBUG:
	std::sort(chosenCameras.begin(), chosenCameras.end());
	for (int i=0; i<chosenCameras.size(); i++) {
		printf("  main camera %i, side cameras ", chosenCameras[i].first);
		for (int j=0; j<chosenCameras[i].second.size(); j++) {
			printf("%i, ", chosenCameras[i].second[j]);
		}
		printf("\n");
	}
}
/*
void Heuristic::chooseCameras(const Mat points, const Mat indices)
{
	chosenCameras.clear();
	for (int i=0; i < config->frameCount(); i += 33) {
		std::vector<int> linked;
		if (i - 25 >= 0)
			linked.push_back(i-25);
		if (i - 20 >= 0)
			linked.push_back(i-20);
		if (i - 15 >= 0)
			linked.push_back(i-15);
		if (i - 10 >= 0)
			linked.push_back(i-10);
		if (i + 10 < config->frameCount())
			linked.push_back(i+10);
		if (i + 15 < config->frameCount())
			linked.push_back(i+15);
		if (i + 20 < config->frameCount())
			linked.push_back(i+20);
		if (i + 25 < config->frameCount())
			linked.push_back(i+25);
		chosenCameras.push_back(numberedVector(i, linked));
	}
}
*/
int Heuristic::beginMain()
{ // initialize and return frame number for first main camera
	if (chosenCameras.size() == 0)
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx = 0].first;
}
int Heuristic::nextMain()
{ // return frame number for next main camera
	if (++mainIdx < chosenCameras.size())
		return chosenCameras[mainIdx].first;
	else
		return Heuristic::sentinel;
}
int Heuristic::beginSide(int imain)
{ // initialize and return frame number for first side camera
	if (imain != chosenCameras[mainIdx].first || chosenCameras[mainIdx].second.size() == 0)
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx].second[sideIdx = 0];
}
int Heuristic::nextSide(int imain)
{ // return frame number for next side camera
	if (imain != chosenCameras[mainIdx].first || ++sideIdx >= chosenCameras[mainIdx].second.size())
		return Heuristic::sentinel;
	else
		return chosenCameras[mainIdx].second[sideIdx];
}
void Heuristic::logAlpha(float alpha)
{
	alphaVals.push_back(alpha);
}
