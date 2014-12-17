#define __CL_ENABLE_EXCEPTIONS
#include <iostream>
#include <string>
#include <fstream>
#include <CL/cl.hpp>
#include <sys/time.h>
#include <boost/format.hpp>

using namespace cl;
using namespace std;
using namespace boost;

/*
 * Always use row-major
 */

struct timeval tpstart, tpend;
double timeuse;

void logTime(std::string message);

int main(int argc, char *argv[]) {

	gettimeofday(&tpstart, NULL);

	if (argc < 3) {
		cout << "Usage: ./matrix-image (rank) (slice)" << endl;
		return -1;
	}

	const int RANK = stoi(argv[1]);
	const int SLICE = stoi(argv[2]);
	const int SIZE = RANK / SLICE;

	if(RANK != SLICE * SIZE){
		cout << format("Matrix with rank %1% can't be slice into %2%*%2% parts.") % RANK % SLICE << endl;
		return -2;
	}

	logTime("Generating Matrix...");
	float** A = new float*[SLICE * SLICE];
	float** B = new float*[SLICE * SLICE];
	float** C = new float*[SLICE * SLICE];

	for(int i = 0; i < SLICE * SLICE; i++){
		A[i] = new float[SIZE * SIZE];
		B[i] = new float[SIZE * SIZE];
		C[i] = NULL;
		for(int j = 0; j < SIZE * SIZE; j++){
			A[i][j] = rand() % 100;
			B[i][j] = rand() % 100;
		}
	}

	try {
		logTime("Initializing OpenCL...");
		std::vector<Platform> platforms;
		Platform::get(&platforms);

		cl_context_properties cps[3] = {
			CL_CONTEXT_PLATFORM,
			(cl_context_properties) (platforms[0]) (),
			0
		};
		Context context(CL_DEVICE_TYPE_GPU, cps);

		std::vector<Device> devices = context.getInfo<CL_CONTEXT_DEVICES>();

		CommandQueue queue = CommandQueue(context, devices[0]);

		std::string code;
		code += "__constant sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_NONE | CLK_FILTER_NEAREST;";
		code += (format("__constant int SIZE = %1%;") % SIZE).str();
		code +=	"__kernel void prod(";

		for(int i = 0; i < SLICE; i++){
			code += (format("__read_only image2d_t A%1%, __read_only image2d_t B%1%, ") % i).str();
		}

		code += (format(
				"__write_only image2d_t C) { \
					const int col = get_global_id(0); \
					const int row = get_global_id(1); \
					float sum = 0; \
					int localID = get_local_id(0); \
					int localSize = get_local_size(0); \
					int cursor; \
					__local float cacheA[%1%];") % SIZE).str();

		for(int j = 0; j < SLICE; j++){
			code += (format(
					"for(cursor = 0; cursor < SIZE; cursor += localSize){ \
						cacheA[cursor + localID] = read_imagef(A%1%, sampler, (int2)(cursor + localID, row)).x; \
					} \
					if(cursor + localID < SIZE){ \
						cacheA[cursor + localID] = read_imagef(A%1%, sampler, (int2)(cursor + localID, row)).x; \
					} \
					barrier(CLK_LOCAL_MEM_FENCE); \
					for (int i = 0; i < SIZE; i++) { \
						sum += cacheA[i] * read_imagef(B%1%, sampler, (int2)(col, i)).x; \
					}") % j).str();
		}
		code += 	"write_imagef(C, (int2)(col, row), sum); \
				}";

		Program::Sources source(1, make_pair(code.c_str(), code.length() + 1));

		Program program = Program(context, source);
		program.build(devices);
		Kernel kernel(program, "prod");

		logTime("Finish initialize OpenCL");

		ImageFormat format(CL_R, CL_FLOAT);

		cl::size_t<3> origin;
		origin[0] = 0;
		origin[1] = 0;
		origin[2] = 0;
		
		cl::size_t<3> region;
		region[0] = SIZE;
		region[1] = SIZE;
		region[2] = 1;

		::size_t* mapSize = new ::size_t(SIZE * sizeof(float));

		Image2D** matrixA = new Image2D*[SLICE];
		Image2D** matrixB = new Image2D*[SLICE];
		Image2D* matrixC = NULL;

		for(int row = 0; row < SLICE; row++){
			for(int i = 0; i < SLICE; i++){
				matrixA[i] = new Image2D(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, format, SIZE, SIZE, 0, A[row * SLICE + i]);
				kernel.setArg(2 * i, *(matrixA[i]));
			}
			for(int col = 0; col < SLICE; col++){
				for(int i = 0; i < SLICE; i++){
					matrixB[i] = new Image2D(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, format, SIZE, SIZE, 0, B[i * SLICE + col]);
					kernel.setArg(2 * i + 1, *(matrixB[i]));
				}
				matrixC = new Image2D(context, CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR, format, SIZE, SIZE);
				kernel.setArg(2 * SLICE, *matrixC);

				logTime((boost::format("Start Computation Block (%1%, %2%)...") % row % col).str());
				queue.enqueueNDRangeKernel(kernel, NullRange, NDRange(SIZE, SIZE), NullRange);
				queue.finish();
				logTime((boost::format("Finish Computation Block (%1%, %2%)") % row % col).str());
				
				C[row * SLICE + col] = (float*) queue.enqueueMapImage(*matrixC, CL_TRUE, CL_MAP_READ, origin, region, mapSize, NULL);

				for(int i = 0; i < SLICE; i++){
					delete matrixB[i];
				}
				delete matrixC;
			}
			for(int i = 0; i < SLICE; i++){
				delete matrixA[i];
			}
		}

		delete mapSize;

		if(argc < 4){
			ofstream outa("a.txt");
			ofstream outb("b.txt");
			ofstream outc("c.txt");

			for(int blockRow = 0; blockRow < SLICE; blockRow++){
				for(int row = 0; row < SIZE; row++){
					for(int blockCol = 0; blockCol < SLICE; blockCol++){
						for(int col = 0; col < SIZE; col++){
							outa << A[blockRow * SLICE + blockCol][row * SIZE + col] << " ";
							outb << B[blockRow * SLICE + blockCol][row * SIZE + col] << " ";
							outc << C[blockRow * SLICE + blockCol][row * SIZE + col] << " ";
						}
					}
					outa << endl;
					outb << endl;
					outc << endl;
				}
			}
		}

	} catch(Error error) {
		cout << error.what() << "(" << error.err() << ")" << endl;
	}

	for(int i = 0; i < SLICE * SLICE; i++){
		delete[] A[i];
		delete[] B[i];
	}
	delete[] A;
	delete[] B;
	delete[] C;

	return 0;
}

void logTime(std::string message) {
	gettimeofday(&tpend, NULL);
	timeuse = 1000000 * (tpend.tv_sec - tpstart.tv_sec) + tpend.tv_usec - tpstart.tv_usec;
	timeuse /= 1000000;
	cout << "[" << timeuse << "] " << message << endl;
}