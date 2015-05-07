#ifndef MNI_RECONSTRUCTION_HPP
#define MNI_RECONSTRUCTION_HPP
#include "gqi_process.hpp"
#include "mapping/fa_template.hpp"
#include "basic_voxel.hpp"
#include "basic_process.hpp"
#include "odf_decomposition.hpp"
#include "odf_deconvolusion.hpp"
#include "gqi_process.hpp"

extern fa_template fa_template_imp;

struct terminated_class {
    unsigned int total;
    mutable unsigned int now;
    mutable bool terminated;
    terminated_class(int total_):total(total_),now(0),terminated(false){}
    bool operator!() const
    {
        terminated = prog_aborted();
        return check_prog(std::min(now++,total-1),total);
    }
    ~terminated_class()
    {
        check_prog(total,total);
    }
};


class DWINormalization  : public BaseProcess
{
protected:
    std::auto_ptr<image::reg::bfnorm_mapping<float,3> > mni;
    image::geometry<3> src_geo;
    image::geometry<3> des_geo;
    int b0_index;
protected:
    image::transformation_matrix<3,float> affine;
protected:
    image::basic_image<float,3> VG,VF;
    double VGvs[3];
protected:
    image::vector<3,int> bounding_box_lower;
    image::vector<3,int> bounding_box_upper;
    image::vector<3,int> des_offset;// = {6,7,11};	// the offset due to bounding box
    image::vector<3,float> scale;
    double trans_to_mni[16];
protected:
    float voxel_volume_scale;
    std::vector<float> jdet;
    std::vector<float> mx,my,mz;
protected:
    typedef image::const_pointer_image<unsigned short,3> point_image_type;
    std::vector<point_image_type> ptr_images;
    std::vector<image::vector<3,float> > q_vectors_time;

public:
    virtual void init(Voxel& voxel)
    {
        if(voxel.vs[0] == 0.0 ||
                voxel.vs[1] == 0.0 ||
                voxel.vs[2] == 0.0)
            throw std::runtime_error("No spatial information found in src file. Recreate src file or contact developer for assistance");
        bool export_intermediate = false;
        begin_prog("normalization");

        VG = fa_template_imp.I;
        VF = voxel.qa_map;
        image::basic_image<float,3> VF2 = voxel.dwi_sum;


        image::filter::gaussian(VF);
        image::filter::gaussian(VF2);
        VF -= image::segmentation::otsu_threshold(VF);
        VF2 -= image::segmentation::otsu_threshold(VF2);
        image::lower_threshold(VF,0.0);
        image::lower_threshold(VF2,0.0);

        src_geo = VF.geometry();

        image::normalize(VG,1.0);
        image::normalize(VF,1.0);
        image::normalize(VF2,1.0);


        VGvs[0] = std::fabs(fa_template_imp.tran[0]);
        VGvs[1] = std::fabs(fa_template_imp.tran[5]);
        VGvs[2] = std::fabs(fa_template_imp.tran[10]);

        const unsigned int num_reg_type = 6;
        int reg_type[num_reg_type],cost_type[num_reg_type];
        reg_type[0] = image::reg::rigid_body;
        reg_type[1] = image::reg::rigid_scaling;
        reg_type[2] = image::reg::affine;
        reg_type[3] = image::reg::rigid_body;
        reg_type[4] = image::reg::rigid_scaling;
        reg_type[5] = image::reg::affine;
        cost_type[0] = 0;
        cost_type[1] = 0;
        cost_type[2] = 0;
        cost_type[3] = 1;
        cost_type[4] = 1;
        cost_type[5] = 1;
        voxel_volume_scale = (voxel.vs[0]*voxel.vs[1]*voxel.vs[2])/(VGvs[0]*VGvs[1]*VGvs[2]);

        image::transformation_matrix<3,float> affine_buf[num_reg_type];
        std::auto_ptr<image::reg::bfnorm_mapping<float,3> > mni_buf[num_reg_type];
        float r2_results[num_reg_type];
        begin_prog("registration...may take a long time");
        bool terminated = false;
        for(unsigned int reg_iter = 0;check_prog(reg_iter,num_reg_type);++reg_iter)
        {
            image::affine_transform<3,float> arg_min;
            // VG: FA TEMPLATE
            // VF: SUBJECT QA
            arg_min.scaling[0] = voxel.vs[0] / VGvs[0];
            arg_min.scaling[1] = voxel.vs[1] / VGvs[1];
            arg_min.scaling[2] = voxel.vs[2] / VGvs[2];
            image::reg::align_center(VF,VG,arg_min);

        // calculate center of mass


            if(export_intermediate)
            {
                VG.save_to_file<image::io::nifti>("VG.nii.gz");
                VF.save_to_file<image::io::nifti>("VF.nii.gz");
                VF2.save_to_file<image::io::nifti>("VF2.nii.gz");
            }

            if(voxel.qsdr_trans.data[0] != 0.0) // has manual reg data
                affine_buf[reg_iter] = voxel.qsdr_trans;
            else
            {
                if(cost_type[reg_iter])
                    image::reg::linear(VF2,VG,arg_min,reg_type[reg_iter],image::reg::mutual_information(),terminated);
                else
                    image::reg::linear(VF,VG,arg_min,reg_type[reg_iter],image::reg::square_error(),terminated);
                affine_buf[reg_iter] = image::transformation_matrix<3,float>(arg_min,VF.geometry(),VG.geometry());
            }
            affine_buf[reg_iter].inverse();
            image::basic_image<float,3> VFF(VG.geometry());
            image::resample(VF,VFF,affine_buf[reg_iter],image::cubic);

            //linear regression
            {
                std::vector<float> x,y;
                x.reserve(VG.size());
                y.reserve(VG.size());
                for(unsigned int index = 0;index < VG.size();++index)
                    if(VG[index] > 0)
                    {
                        x.push_back(VFF[index]);
                        y.push_back(VG[index]);
                    }
                std::pair<double,double> r = image::linear_regression(x.begin(),x.end(),y.begin());
                for(unsigned int index = 0;index < VG.size();++index)
                    if(VG[index] > 0)
                        VFF[index] = std::max<float>(0,VFF[index]*r.first+r.second);
                    else
                        VFF[index] = 0;
            }


            if(export_intermediate)
                VFF.save_to_file<image::io::nifti>("VFF.nii.gz");

            try
            {

                unsigned int factor = voxel.reg_method + 1;
                mni_buf[reg_iter].reset(new image::reg::bfnorm_mapping<float,3>(VG.geometry(),image::geometry<3>(factor*7,factor*9,factor*7)));
                multi_thread_reg(*(mni_buf[reg_iter]).get(),VG,VFF,voxel.voxel_data.size(),terminated);

            }
            catch(...)
            {
                throw std::runtime_error("Registration failed due to memory insufficiency.");
            }
            {
                std::vector<float> x,y;
                x.reserve(VG.size());
                y.reserve(VG.size());
                for (image::pixel_index<3> index;index.index() < VG.size();index.next(VG.geometry()))
                    if(VG[index.index()] > 0)
                    {
                        image::vector<3,double> pos;
                        (*(mni_buf[reg_iter]).get())(index,pos);
                        float value = 0.0;
                        image::estimate(VFF,pos,value,image::linear);
                        x.push_back(VG[index.index()]);
                        y.push_back(value);
                    }

                r2_results[reg_iter] = image::correlation(x.begin(),x.end(),y.begin());
                r2_results[reg_iter] *= r2_results[reg_iter];
                std::cout << "R2 = " << r2_results[reg_iter] << std::endl;

                if(export_intermediate)
                {
                    image::basic_image<float,3> VFFF(VG.geometry());
                    image::resample(VFF,VFFF,*(mni_buf[reg_iter]).get(),image::cubic);
                    VFFF.save_to_file<image::io::nifti>("VFFF.nii.gz");
                }
            }
        }
        if(prog_aborted())
            throw std::runtime_error("Reconstruction canceled");
        int reg_choice = std::max_element(r2_results,r2_results+3)-r2_results;
        std::cout << "reg_type=" << reg_choice << std::endl;
        affine = affine_buf[reg_choice];
        voxel.R2 = r2_results[reg_choice];
        mni.reset(mni_buf[reg_choice].release());

        // setup output bounding box
        {
            //setBoundingBox(-78,-112,-50,78,76,85,1.0);
            float voxel_size = voxel.param[1];
            bounding_box_lower[0] = std::floor(-78.0/voxel_size+0.5)*voxel_size;
            bounding_box_lower[1] = std::floor(-112.0/voxel_size+0.5)*voxel_size;
            bounding_box_lower[2] = std::floor(-50.0/voxel_size+0.5)*voxel_size;
            bounding_box_upper[0] = std::floor(78.0/voxel_size+0.5)*voxel_size;
            bounding_box_upper[1] = std::floor(76.0/voxel_size+0.5)*voxel_size;
            bounding_box_upper[2] = std::floor(85.0/voxel_size+0.5)*voxel_size;
            des_geo[0] = (bounding_box_upper[0]-bounding_box_lower[0])/voxel_size+1;//79
            des_geo[1] = (bounding_box_upper[1]-bounding_box_lower[1])/voxel_size+1;//95
            des_geo[2] = (bounding_box_upper[2]-bounding_box_lower[2])/voxel_size+1;//69

            // DSI Studio use radiology convention, the MNI coordinate of the x and y are flipped
            des_offset[0] = (fa_template_imp.I.width()-1)*VGvs[0]+fa_template_imp.tran[3]-bounding_box_upper[0];
            des_offset[1] = (fa_template_imp.I.height()-1)*VGvs[1]+fa_template_imp.tran[7]-bounding_box_upper[1];
            des_offset[2] = bounding_box_lower[2]-fa_template_imp.tran[11];

            // units in template space
            des_offset[0] /= VGvs[0];
            des_offset[1] /= VGvs[1];
            des_offset[2] /= VGvs[2];

            scale[0] = voxel_size/VGvs[0];
            scale[1] = voxel_size/VGvs[1];
            scale[2] = voxel_size/VGvs[2];

            // setup transformation matrix
            std::fill(trans_to_mni,trans_to_mni+16,0.0);
            trans_to_mni[15] = 1.0;
            trans_to_mni[0] = -voxel_size;
            trans_to_mni[5] = -voxel_size;
            trans_to_mni[10] = voxel_size;
            trans_to_mni[3] = bounding_box_upper[0];
            trans_to_mni[7] = bounding_box_upper[1];
            trans_to_mni[11] = bounding_box_lower[2];
        }

        // setup mask
        {
            // set the current mask to template space
            voxel.dim = des_geo;
            voxel.image_model->mask.resize(des_geo);
            std::fill(voxel.image_model->mask.begin(),voxel.image_model->mask.end(),0);
            for(image::pixel_index<3> index; index.is_valid(des_geo); index.next(des_geo))
            {
                image::vector<3,float> mni_pos(index);
                mni_pos *= voxel.param[1];
                mni_pos[0] /= VGvs[0];
                mni_pos[1] /= VGvs[1];
                mni_pos[2] /= VGvs[2];
                mni_pos += des_offset;
                mni_pos += 0.5;
                mni_pos.floor();
                if(fa_template_imp.I.geometry().is_valid(mni_pos) &&
                        fa_template_imp.I.at(mni_pos[0],mni_pos[1],mni_pos[2]) > 0.0)
                    voxel.image_model->mask[index.index()] = 1;
            }
        }




        b0_index = -1;
        if(voxel.half_sphere)
            for(unsigned int index = 0; index < voxel.bvalues.size(); ++index)
                if(voxel.bvalues[index] == 0)
                    b0_index = index;

        ptr_images.clear();
        for (unsigned int index = 0; index < voxel.image_model->dwi_data.size(); ++index)
            ptr_images.push_back(image::make_image(src_geo,voxel.image_model->dwi_data[index]));


        std::fill(voxel.vs.begin(),voxel.vs.end(),voxel.param[1]);

        voxel.csf_pos1 = mni_to_voxel_index(6,0,18);
        voxel.csf_pos2 = mni_to_voxel_index(-6,0,18);
        voxel.z0 = 0.0;

        // output mapping
        if(voxel.output_jacobian)
            jdet.resize(voxel.dim.size());

        if(voxel.output_mapping)
        {
            mx.resize(voxel.dim.size());
            my.resize(voxel.dim.size());
            mz.resize(voxel.dim.size());
        }
    }

    image::vector<3,int> mni_to_voxel_index(int x,int y,int z) const
    {
        x = bounding_box_upper[0]-x;
        y = bounding_box_upper[1]-y;
        z -= bounding_box_lower[2];
        x /= scale[0];
        y /= scale[1];
        z /= scale[2];
        return image::vector<3,int>(x,y,z);
    }

    void get_jacobian(const image::vector<3,double>& pos,float* jacobian)
    {
        float M[9];
        image::reg::bfnorm_get_jacobian(*mni.get(),pos,M);
        image::matrix::product(affine.scaling_rotation,M,jacobian,image::dim<3,3>(),image::dim<3,3>());
    }

    template<typename interpolation_type>
    void interpolate_dwi(Voxel& voxel, VoxelData& data,
                         const image::vector<3,double>& pos,
                         const image::vector<3,double>& Jpos,interpolation_type)
    {
        interpolation_type interpolation;
        if(!interpolation.get_location(src_geo,Jpos))
        {
            std::fill(data.space.begin(),data.space.end(),0);
            std::fill(data.jacobian,data.jacobian+9,0.0);
            return;
        }
        data.space.resize(ptr_images.size());
        for (unsigned int i = 0; i < ptr_images.size(); ++i)
            interpolation.estimate(ptr_images[i],data.space[i]);
        if(voxel.half_sphere && b0_index != -1)
            data.space[b0_index] /= 2.0;

        get_jacobian(pos,data.jacobian);
        if(!voxel.grad_dev.empty())
        {
            /*
            new_bvecs = (I+grad_dev) * bvecs;
            */

            float grad_dev[9];
            for(unsigned int i = 0; i < 9; ++i)
                interpolation.estimate(voxel.grad_dev[i],grad_dev[i]);
            // this grad_dev matrix is rotated
            // add identity matrix
            grad_dev[0] += 1.0;
            grad_dev[4] += 1.0;
            grad_dev[8] += 1.0;
            // bvec is flipped at y direction
            // 1  0  0         1  0  0
            //[0 -1  0] *Grad*[0 -1  0]
            // 0  0  1         0  0  1
            grad_dev[1] = -grad_dev[1];
            grad_dev[3] = -grad_dev[3];
            grad_dev[5] = -grad_dev[5];
            grad_dev[7] = -grad_dev[7];
            float new_j[9];
            image::matrix::product(grad_dev,data.jacobian,new_j,image::dim<3,3>(),image::dim<3,3>());
            std::copy(new_j,new_j+9,data.jacobian);
            //  <G*b_vec,J*odf>
            //  = trans(b_vec)*trans(G)*J*odf
        }
    }

    virtual void run(Voxel& voxel, VoxelData& data)
    {
        image::vector<3,double> pos(image::pixel_index<3>(data.voxel_index,voxel.dim)),Jpos;
        pos[0] *= scale[0];
        pos[1] *= scale[1];
        pos[2] *= scale[2];
        pos += des_offset;
        pos += 0.5;
        pos.floor();
        (*mni.get())(pos,Jpos);
        affine(Jpos);

        // output mapping position
        if(voxel.output_mapping)
        {
            mx[data.voxel_index] = Jpos[0];
            my[data.voxel_index] = Jpos[1];
            mz[data.voxel_index] = Jpos[2];
        }
        switch(voxel.interpo_method)
        {
        case 0:
            interpolate_dwi(voxel,data,pos,Jpos,image::interpolation<image::linear_weighting,3>());
            break;
        case 1:
            interpolate_dwi(voxel,data,pos,Jpos,image::interpolation<image::gaussian_radial_basis_weighting,3>());
            break;
        case 2:
            interpolate_dwi(voxel,data,pos,Jpos,image::cubic_interpolation<3>());
            break;
        }
        data.jdet = std::abs(image::matrix::determinant(data.jacobian,image::dim<3,3>())*voxel_volume_scale);
        if(voxel.output_jacobian)
            jdet[data.voxel_index] = data.jdet;
    }
    virtual void end(Voxel& voxel,gz_mat_write& mat_writer)
    {
        voxel.image_model->mask.resize(src_geo);
        voxel.dim = src_geo;
        if(voxel.output_jacobian)
            mat_writer.write("jdet",&*jdet.begin(),1,jdet.size());
        if(voxel.output_mapping)
        {
            mat_writer.write("mx",&*mx.begin(),1,mx.size());
            mat_writer.write("my",&*my.begin(),1,my.size());
            mat_writer.write("mz",&*mz.begin(),1,mz.size());
        }
        mat_writer.write("trans",&*trans_to_mni,4,4);
        mat_writer.write("R2",&voxel.R2,1,1);
    }

};

class EstimateZ0_MNI : public BaseProcess
{
public:
    void init(Voxel& voxel)
    {
        voxel.z0 = 0.0;
    }
    void run(Voxel& voxel, VoxelData& data)
    {
        // perform csf cross-subject normalization
        {
            image::vector<3,int> cur_pos(image::pixel_index<3>(data.voxel_index,voxel.dim));
            if((cur_pos-voxel.csf_pos1).length() <= 1.0 || (cur_pos-voxel.csf_pos2).length() <= 1.0)
            {
                float odf_dif = *std::min_element(data.odf.begin(),data.odf.end());
                odf_dif /= data.jdet;
                if(odf_dif > voxel.z0)
                    voxel.z0 = odf_dif;
            }
        }
    }
    void end(Voxel& voxel,gz_mat_write& mat_writer)
    {
        if(voxel.z0 == 0.0)
            voxel.z0 = 1.0;
        mat_writer.write("z0",&voxel.z0,1,1);
    }

};

class QSDR  : public BaseProcess
{
public:
    BalanceScheme balance_scheme;
    float angle_variance;
    double r2_base_function(double theta)
    {
        if(std::abs(theta) < 0.000001)
            return 1.0/3.0;
        return (2*std::cos(theta)+(theta-2.0/theta)*std::sin(theta))/theta/theta;
    }
protected:
    std::vector<image::vector<3,double> > q_vectors_time;
public:
    virtual void init(Voxel& voxel)
    {
        float sigma = voxel.param[0];
        q_vectors_time.resize(voxel.bvalues.size());
        for (unsigned int index = 0; index < voxel.bvalues.size(); ++index)
        {
            q_vectors_time[index] = voxel.bvectors[index];
            q_vectors_time[index] *= std::sqrt(voxel.bvalues[index]*0.01506);// get q in (mm) -1
            q_vectors_time[index] *= sigma;
        }
    }

    virtual void run(Voxel& voxel, VoxelData& data)
    {
        std::vector<float> sinc_ql(data.odf.size()*data.space.size());
        for (unsigned int j = 0,index = 0; j < data.odf.size(); ++j)
        {
            image::vector<3,double> dir(voxel.ti.vertices[j]),from;
            image::matrix::vector_product(data.jacobian,dir.begin(),from.begin(),image::dim<3,3>());
            from.normalize();
            if(voxel.r2_weighted)
                for (unsigned int i = 0; i < data.space.size(); ++i,++index)
                    sinc_ql[index] = r2_base_function(q_vectors_time[i]*from);
            else
                for (unsigned int i = 0; i < data.space.size(); ++i,++index)
                    sinc_ql[index] = boost::math::sinc_pi(q_vectors_time[i]*from);

        }
        image::matrix::vector_product(&*sinc_ql.begin(),&*data.space.begin(),&*data.odf.begin(),
                                      image::dyndim(data.odf.size(),data.space.size()));
        std::for_each(data.odf.begin(),data.odf.end(),boost::lambda::_1 *= data.jdet);

    }
    virtual void end(Voxel& voxel,gz_mat_write& mat_writer)
    {

    }

};

#endif//MNI_RECONSTRUCTION_HPP
